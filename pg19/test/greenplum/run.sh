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
# Part of Cloudberry's greenplum_schedule, on a cluster: M2's tests.
#
# src/test/regress/greenplum_schedule is what Cloudberry runs against its demo
# cluster -- a coordinator and three segments -- after PostgreSQL's
# parallel_schedule, in one database.  The port runs a part of it the same
# way, on a coordinator and three segments of its own, with every table the
# tests make distributed as Cloudberry distributes it: manifest says of each
# test of the schedule whether it runs and, if not, why.  Before them run
# PostgreSQL 19's test_setup, which makes the tables PostgreSQL's tests leave
# for Cloudberry's (onek, tenk1, int4_tbl, ...), and the port's own setup,
# sql/gp_setup.sql, which creates the extensions.
#
# The tests run in the groups the manifest puts them in, each group on a
# cluster of its own, test_setup and gp_setup first, and the groups of a pass
# side by side, each in the schedule's order, as the isolation2 suite runs
# its groups; what a pass reports is in the manifest's order, whichever
# group finished first.  A test reads only what the tests before it in its
# group made, so a test that reads another's tables is in that one's group.
#
# Cloudberry's files are changed as the singlenode suite changes them:
# input/ and output/ .source files converted as Cloudberry's pg_regress
# converts them, and a setting the port has spelled as the port spells it;
# and a program of Cloudberry's suite is run from where the port installs it.
# Each test is compared as Cloudberry's pg_regress compares it: gpdiff.pl,
# under Cloudberry's init_file and the port's, plans not compared -- the
# port's plans are ORCA's or the planner's with the port's Motions, and what
# is compared is the answers -- or exactly a difference in cloudberry/,
# reviewed and kept.  Two passes, as the singlenode suite has: the planner
# (gp.optimizer = off) and ORCA.
#

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CB="${CB_REGRESS_DIR:-/cb/src/test/regress}"
PGSUITE="${PG_REGRESS_SUITE:-/cb/pgregress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"
# A suite of this form from another directory, CB_REGRESS_DIR -- PAX's copy
# of Cloudberry's suite, which ../pax_regress runs through this -- has its
# own manifest, differences kept, init_file and tests of the port's
# (GP_SUITE_DIR, whose sql/ and expected/ go with this directory's); the
# Perl that compares the output from Cloudberry's suite (CB_GPDIFF_DIR), whose
# init_file goes with the suite's; the modules and settings its clusters
# have besides (GP_PRELOAD_MORE, GP_SETTINGS); and the tests each group's
# pass begins with (GP_SETUP).
SUITE_DIR="${GP_SUITE_DIR:-$HERE}"
GPDIFF="${CB_GPDIFF_DIR:-$CB}"
SETUP="${GP_SETUP:-test_setup gp_setup}"

if [ ! -f "$CB/greenplum_schedule" ] || [ ! -f "$PGSUITE/sql/test_setup.sql" ] ||
   [ ! -f "$PGSUITE/regress.so" ] || [ ! -x "$PG_REGRESS" ] ||
   [ ! -f "$GPDIFF/gpdiff.pl" ]; then
	echo "a test suite, pg_regress or gpdiff.pl is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-greenplum-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbg-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec
# (see the singlenode suite).
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-greenplum-XXXXXX")"
if ! . "$HERE/../gpmgmt/tools.sh" "$EXEC"; then
	echo "gpMgmt, or the Python it needs, is not installed; skipping"
	rm -rf "$WORK" "$SOCK" "$EXEC"
	exit 77
fi
BASEPORT="${PGPORT:-$((7300 + RANDOM % 200))}"
NODES=4					# a coordinator and Cloudberry's three segments
# gp_matview after gp_sql, whose hooks it runs outside of, as the dump suite
# has it: an incremental view's distribution is an option gp_sql reads; and
# gp_task, whose scheduler refreshes a dynamic table
PRELOAD="gp_core,gp_orca,gp_sql,gp_ao,gp_exttable,gp_security,gp_resource,gp_matview,gp_task${GP_PRELOAD_MORE:+,$GP_PRELOAD_MORE}"
# The modules and settings the clusters have besides (GP_PRELOAD_MORE,
# GP_SETTINGS); and the interconnect every node has, GP_INTERCONNECT, tcp
# by default -- the ic_udp2 and ic_proxy suites run this one with theirs --
# which each node is checked to have once it is up.  The proxies listen on
# TCP whatever the nodes do, each on its node's port and IC_PROXY_OFFSET
# more, a range no suite's nodes use (the isolation2 suite's is 2000 above).
INTERCONNECT="${GP_INTERCONNECT:-tcp}"
IC_PROXY_OFFSET=12000
SECRET="greenplum-schedule-$RANDOM$RANDOM$RANDOM"

# The tests the manifest runs -- Cloudberry's, and the port's (port:name)
# among them where it puts them -- in its order, the group each is in, and
# the pass it runs in where its line names one ("-" for both); the groups,
# in the order they first appear.
run_tests=(); run_group=(); run_pass=()
while read -r kind t g p; do
	[ "$kind" = port ] && t="port:$t"
	run_tests+=("$t"); run_group+=("$g"); run_pass+=("$p")
done < <(awk '$1 == "group" { g = $2 }
			  $1 == "run" || $1 == "port" {
				  print $1, $2, (g == "" ? "main" : g), ($1 == "run" && NF > 2 ? $3 : "-") }' "$SUITE_DIR/manifest")
for p in "${run_pass[@]}"; do
	case "$p" in
		-|planner|orca) ;;
		*) echo "greenplum: a run line of the manifest names the pass \"$p\", which is none" >&2; exit 1 ;;
	esac
done
groups=()
for g in "${run_group[@]}"; do
	[[ " ${groups[*]} " == *" $g "* ]] || groups+=("$g")
done

# Node n of the gi-th group's cluster: its data directory, its port and the
# socket directory it listens on alone.
node_dir()  { echo "$WORK/$1/node$2"; }
node_port() { echo $((BASEPORT + $1 * NODES + $2)); }
node_sock() { echo "$SOCK/$1/n$2"; }
# A group whose name begins "mirrors" has Cloudberry's demo cluster whole, as
# isolation2's groups of the name have it (../isolation2/run.sh): a mirror
# for each segment, a hot standby streaming from its primary, which FTS on
# the coordinator watches, and a standby coordinator -- the mirrors dbids 5
# to 7 and the standby 8, as Cloudberry's are.  Content c's mirror in the
# gi-th group, and the group's standby.
has_mirrors()  { [[ "$1" == mirrors* ]]; }
mirror_dir()   { echo "$WORK/$1/mirror$2"; }
mirror_port()  { echo $((BASEPORT + 300 + $1 * NODES + $2)); }
mirror_sock()  { echo "$SOCK/$1/m$2"; }
standby_dir()  { echo "$WORK/$1/standby"; }
standby_port() { echo $((BASEPORT + 100 + $1)); }
standby_sock() { echo "$SOCK/$1/s"; }
# A group whose name begins "tcp" has its nodes reach one another over TCP,
# on 127.0.0.1, as Cloudberry's nodes do, where the others' use Unix
# sockets: a test that reads the TCP options of the coordinator's
# connections to the segments (gp_dispatch_keepalives) has them there.  The
# tests still connect to the coordinator through its socket.
over_tcp()     { [[ "$1" == tcp* ]]; }
node_host()    { if over_tcp "$1"; then echo 127.0.0.1; else node_sock "$1" "$2"; fi; }
# The proxies' addresses of a group (GP_INTERCONNECT=proxy): every node,
# mirrors and the standby too, as Cloudberry's gp_interconnect_proxy_addresses
# lists them -- dbid:content:host:port, in dbid order.
proxy_addresses() {
	local g="$1" gi="$2" n out=""
	for n in $(seq 0 $((NODES - 1))); do
		out="$out,$((n + 1)):$((n - 1)):127.0.0.1:$(($(node_port "$gi" "$n") + IC_PROXY_OFFSET))"
	done
	if has_mirrors "$g"; then
		for n in $(seq 0 $((NODES - 2))); do
			out="$out,$((NODES + 1 + n)):$n:127.0.0.1:$(($(mirror_port "$gi" "$n") + IC_PROXY_OFFSET))"
		done
		out="$out,$((2 * NODES)):-1:127.0.0.1:$(($(standby_port "$gi") + IC_PROXY_OFFSET))"
	fi
	echo "${out#,}"
}
# The superuser is Cloudberry's demo cluster's, gpadmin, as the singlenode
# suite's is: Cloudberry's expected output names it.
export PGUSER=gpadmin

cleanup() {
	local w g n
	for w in $(jobs -p); do kill "$w" 2> /dev/null; done
	for g in "${groups[@]}"; do
		for n in $(seq 0 $((NODES - 1))) standby $(seq -f 'mirror%g' 0 $((NODES - 2))); do
			case "$n" in
				standby|mirror*) d="$WORK/$g/$n" ;;
				*) d="$(node_dir "$g" "$n")" ;;
			esac
			[ -d "$d" ] || continue
			[ -n "${RESULTS_DIR:-}" ] &&
				cp "$d.log" "$RESULTS_DIR/greenplum-$g-node$n.log" 2> /dev/null
			"$BINDIR/pg_ctl" -D "$d" -m immediate stop > /dev/null 2>&1
		done
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

# The manifest's lines of the schedule's tests, less the one of Cloudberry's
# parallel_schedule it runs too -- or every one of another suite's manifest
# (GP_SUITE_DIR).
of_schedule() {
	if [ "$SUITE_DIR" = "$HERE" ]; then
		awk 'NR == FNR { if ($1 == "test:") for (i = 2; i <= NF; i++) s[$i]; next }
			 ($1 == "run" || $1 == "skip") && ($2 in s)' "$CB/greenplum_schedule" "$SUITE_DIR/manifest"
	else
		awk '$1 == "run" || $1 == "skip"' "$SUITE_DIR/manifest"
	fi
}
TITLE="greenplum: part of Cloudberry's greenplum_schedule"
echo "${GP_TITLE:-$TITLE}, on a coordinator and three segments"
printf '  of the %d tests of the schedule the manifest lists: %d run here, %d of them in one pass, in %d groups, %d are skipped\n' \
	"$(of_schedule | wc -l)" \
	"$(of_schedule | awk '$1 == "run"' | wc -l)" \
	"$(of_schedule | awk '$1 == "run" && NF > 2' | wc -l)" "${#groups[@]}" \
	"$(of_schedule | awk '$1 == "skip"' | wc -l)"
echo

# A group's cluster, as run.sh of the cluster suite makes one.
make_cluster() {
	local g="$1" gi="$2" conf="$WORK/$1/gp_cluster.conf" n d
	mkdir -p "$WORK/$g"
	{
		echo "# dbid content role host port datadir"
		for n in $(seq 0 $((NODES - 1))); do
			echo "$((n + 1)) $((n - 1)) p $(node_host "$g" "$n") $(node_port "$gi" "$n") $(node_dir "$g" "$n")"
		done
		if has_mirrors "$g"; then
			for n in $(seq 0 $((NODES - 2))); do
				echo "$((NODES + 1 + n)) $n m $(mirror_sock "$g" "$n") $(mirror_port "$gi" "$n") $(mirror_dir "$g" "$n")"
			done
			echo "$((2 * NODES)) -1 m $(standby_sock "$g") $(standby_port "$gi") $(standby_dir "$g")"
		fi
	} > "$conf"
	for n in $(seq 0 $((NODES - 1))); do
		d="$(node_dir "$g" "$n")"
		mkdir -p "$(node_sock "$g" "$n")"
		"$BINDIR/initdb" -D "$d" -N -U gpadmin --locale=C --encoding=UTF8 > "$d.initdb.log" 2>&1 \
			|| { echo "initdb failed for node $n of group $g"; tail -20 "$d.initdb.log"; return 1; }
		{
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "unix_socket_directories = '$(node_sock "$g" "$n")'"
			echo "listen_addresses = '$(over_tcp "$g" && echo 127.0.0.1)'"
			echo "port = $(node_port "$gi" "$n")"
			# fsync on in a group with mirrors, as Cloudberry's demo cluster
			# has it: alter_db_set_tablespace shows it, and has a mirror's
			# restartpoint sync what a moved database had
			echo "fsync = $(has_mirrors "$g" && echo on || echo off)"
			[ -n "${GP_SETTINGS:-}" ] && echo "$GP_SETTINGS"
			[ -n "${GP_INTERCONNECT:-}" ] && echo "gp.interconnect_type = '$INTERCONNECT'"
			[ "$INTERCONNECT" = proxy ] &&
				echo "gp.interconnect_proxy_addresses = '$(proxy_addresses "$g" "$gi")'"
			echo "gp.cluster_config = '$conf'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			# every transaction that writes on a segment is prepared there, and
			# pg_regress runs up to 20 sessions at once
			echo "max_prepared_transactions = 64"
			# what Cloudberry's postgresql.conf.sample sets, and its demo
			# cluster runs with: a statement's memory is its queue's to give
			echo "gp.resqueue_memory_policy = 'eager_free'"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
			# every statement in the coordinator's log, as gpinitsystem sets
			# it on the cluster Cloudberry's tests run on: log_guc reads
			# them back, and a test that lowers client_min_messages to log
			# sees them (planhints)
			[ "$n" -eq 0 ] && echo "log_statement = 'all'"
			# each statement ORCA would not plan, and why, in the log: the
			# ORCA pass's reasons, totalled below
			[ "$n" -eq 0 ] && echo "gp.optimizer_log_fallback = on"
		} >> "$d/postgresql.auto.conf"
	done
	for n in $(seq 1 $((NODES - 1))) 0; do
		# the mirrors before the coordinator, as gpinitsystem makes them, so
		# that FTS's first probe finds them there
		[ "$n" -eq 0 ] && has_mirrors "$g" && { make_mirrors "$g" "$gi" || return 1; }
		d="$(node_dir "$g" "$n")"
		"$BINDIR/pg_ctl" -D "$d" -l "$d.log" -w -t 60 start > /dev/null 2>&1 \
			|| { echo "node $n of group $g did not start"; tail -20 "$d.log"; return 1; }
	done
	has_mirrors "$g" || return 0

	# The standby coordinator, streaming from the coordinator as
	# gp_walreceiver, as Cloudberry's does (gp_standby.c), a hot standby as
	# the mirrors are.
	d="$(standby_dir "$g")"
	mkdir -p "$(standby_sock "$g")"
	copy_node "$d" "$(node_sock "$g" 0)" "$(node_port "$gi" 0)" "" \
		"$(standby_sock "$g")" "$(standby_port "$gi")" $((2 * NODES)) \
		|| { echo "the standby of group $g could not be made"; return 1; }

	# The pairs in sync, and synchronous replication on, as the tests begin:
	# as FTS says in gp_segment_configuration, which database postgres has
	# once it has gp_core, as isolation2's has it.
	"$PSQL" -X -q -h "$(node_sock "$g" 0)" -p "$(node_port "$gi" 0)" -d postgres \
		-c "CREATE EXTENSION IF NOT EXISTS gp_core" > /dev/null 2>&1
	local synced=
	for _ in $(seq 300); do
		synced=$("$PSQL" -X -q -t -A -h "$(node_sock "$g" 0)" -p "$(node_port "$gi" 0)" -d postgres \
			-c "SELECT gp_request_fts_probe_scan(); SELECT count(*) FROM gp_segment_configuration WHERE content >= 0 AND mode <> 's'" \
			2> /dev/null | tail -1)
		[ "$synced" = 0 ] && break
		sleep 0.2
	done
	[ "$synced" = 0 ] || { echo "the mirrors of group $g did not come in sync"; return 1; }
}

# copy_node <dir> <primary's socket> <port> <slot> <socket> <port> <dbid>: a
# hot standby of a node, copied with pg_basebackup -- from a slot, where one
# is named -- and streaming from it as gp_walreceiver, as Cloudberry's
# mirrors and standby are named; its log beside its data directory, as every
# node's is.
copy_node() {
	local d="$1" from_sock="$2" from_port="$3" slot="$4" sock="$5" port="$6" dbid="$7"
	"$BINDIR/pg_basebackup" -D "$d" -h "$from_sock" -p "$from_port" -X stream -c fast \
		${slot:+-C -S "$slot"} > "$d.basebackup.log" 2>&1 \
		|| { tail -5 "$d.basebackup.log"; return 1; }
	grep -v -E '^(port|unix_socket_directories|gp\.dbid|primary_conninfo|primary_slot_name|hot_standby) ' \
		"$d/postgresql.auto.conf" > "$d.auto"
	{
		cat "$d.auto"
		echo "port = $port"
		echo "unix_socket_directories = '$sock'"
		echo "gp.dbid = $dbid"
		echo "hot_standby = on"
		echo "primary_conninfo = 'host=$from_sock port=$from_port application_name=gp_walreceiver'"
		[ -n "$slot" ] && echo "primary_slot_name = '$slot'"
	} > "$d/postgresql.auto.conf"
	rm -f "$d.auto"
	touch "$d/standby.signal"
	"$BINDIR/pg_ctl" -D "$d" -l "$d.log" -w -t 60 start > /dev/null 2>&1 \
		|| { tail -20 "$d.log"; return 1; }
}

# Each segment's mirror: a copy of its primary, from its slot, as Cloudberry's
# mirrors stream.
make_mirrors() {
	local g="$1" gi="$2" c
	for c in $(seq 0 $((NODES - 2))); do
		mkdir -p "$(mirror_sock "$g" "$c")"
		copy_node "$(mirror_dir "$g" "$c")" "$(node_sock "$g" $((c + 1)))" \
			"$(node_port "$gi" $((c + 1)))" internal_wal_replication_slot \
			"$(mirror_sock "$g" "$c")" "$(mirror_port "$gi" "$c")" $((NODES + 1 + c)) \
			|| { echo "the mirror of content $c of group $g could not be made"; return 1; }
	done
}
t0=$(date +%s)
for gi in "${!groups[@]}"; do
	make_cluster "${groups[$gi]}" "$gi" &
done
for g in "${groups[@]}"; do
	wait -n || exit 1
done
# Every node has the interconnect asked for: a run over udp2 or the proxy
# is never one over tcp unawares.
for gi in "${!groups[@]}"; do
	for n in $(seq 0 $((NODES - 1))); do
		ic=$("$PSQL" -X -q -t -A -h "$(node_sock "${groups[$gi]}" "$n")" -p "$(node_port "$gi" "$n")" \
			-d postgres -c "SHOW gp.interconnect_type" 2>&1)
		[ "$ic" = "$INTERCONNECT" ] ||
			{ echo "node $n of group ${groups[$gi]} has gp.interconnect_type \"$ic\", not $INTERCONNECT"; exit 1; }
	done
done
t1=$(date +%s)

# The settings the port has, and what respells Cloudberry's names for them
# (../respell.pl); the singlenode suite says how.
{
	# the column SHOW names, read as a row's field; SET, RESET and SHOW;
	# current_setting() and set_config(); SHOW's header, the same width
	# spelled either way (see the singlenode suite); an error's naming of a
	# setting, parameter "gp_..."; and gpconfig's -c, -r and -s, which set,
	# remove and show a setting in every node's configuration file
	echo "kinds field set func header param gpconfig"
	PGHOST="$(node_sock "${groups[0]}" 0)" PGPORT="$(node_port 0 0)" \
	"$PSQL" -X -q -t -A -d postgres -c "SELECT name FROM pg_settings WHERE name LIKE 'gp.%' ORDER BY length(name) DESC" |
	while read -r name; do
		short="${name#gp.}"
		case "$short" in
			optimizer*|statement_mem|enable_parallel*|parallel_query_use_streaming_hashagg|\
			enable_groupagg|test_print_*|\
			resource_scheduler|resource_select_only|resource_cleanup_gangs_on_wait|\
			max_resource_queues|max_resource_portals_per_transaction|max_statement_mem|\
			debug_resource_group|runaway_detector_activation_percent|\
			vmem_process_interrupt|explain_memory_verbosity|coredump_on_memerror|\
			debug_print_slice_table|\
			enable_offload_entry_to_qe|debug_dtm_action*|debug_abort_after_distributed_prepared|\
			debug_print_full_dtm|enable_answer_query_using_materialized_views|aqumv_allow_foreign_table)
				cbname="$short" ;;
			*) cbname="gp_$short" ;;
		esac
		echo "map $cbname $name"
	done
	# The setting of the tests' own library (cb_regress.c), which no module
	# defines: test_consume_xids() takes XIDs fast under it, as Cloudberry's
	# GetNewTransactionId() does under debug_burn_xids.
	echo "map debug_burn_xids gp.debug_burn_xids"
	# And a program of Cloudberry's suite that a test runs from the suite's
	# directory, ./extended_protocol_resqueue or ./twophase_pqexecparams, is
	# the one the port builds and installs (meson's hook_tests), run from
	# PATH as the diff is.
	echo 'sed s#^[\\]! \./(extended_protocol_resqueue|twophase_pqexecparams) #\\! \1 #'
	# So is bb_memory_quota's script, $PG_ABS_BUILDDIR/mem_quota_util.py, from
	# PATH (below); it runs its queries in the database it is named, which is
	# regression here.
	echo 'sed s#^[\\]! \$PG_ABS_BUILDDIR/(mem_quota_util\.py) (.*)--dbname=regress #\\! \1 \2--dbname=regression #'
	# PostgreSQL 19's pg_stats has five columns Cloudberry's has not -- the
	# table's OID, the column's number, and three of a range's histograms --
	# so a test's SELECT * of it asks for Cloudberry's fourteen by name, in
	# the test and its expected output alike.
	echo 'sed s#\b(select) \* (from pg_stats)\b#\1 schemaname, tablename, attname, inherited, null_frac, avg_width, n_distinct, most_common_vals, most_common_freqs, histogram_bounds, correlation, most_common_elems, most_common_elem_freqs, elem_count_histogram \2#Ig'
	# aqumv orders rows by c2 - c1 - 1, which all but one of them have the
	# same: which of those comes first is the order the segments' rows reach
	# the coordinator's sort in, which varies from run to run, where
	# Cloudberry's Gather Motion merges the segments' sorted rows in one
	# order.  Compared as the rows they are, atmsort's "-- order none".
	echo 'sed s#^(select c1, c3 from aqumv_t5 where c1 > 90 order by c2 - c1 - 1 asc;)#\1 -- order none#'
	# ic_proxy_socket's PL/Python reads SHOW's row by the setting's name,
	# which the port spells gp.interconnect_*.
	echo 'sed s#\["gp_interconnect_(type|proxy_addresses)"\]#["gp.interconnect_\1"]#g'
	# and the suite's own, where it has any
	[ "$SUITE_DIR" = "$HERE" ] || cat "$SUITE_DIR/respell" 2> /dev/null
} > "$WORK/respell"
respell() { perl "$HERE/../respell.pl" "$WORK/respell" "$@"; }

# Cloudberry's file, as its pg_regress converts it, for a group
convert() {
	local g="$1" f="$2"
	sed -e "s#@abs_srcdir@#$CB#g" \
	    -e "s#@abs_builddir@#$CB#g" \
	    -e "s#@testtablespace@#$WORK/$g/testtablespace#g" \
	    -e "s#@libdir@#$PGSUITE#g" \
	    -e "s#@DLSUFFIX@#.so#g" "$f"
}

# test_setup reads its data from the directory it runs from, --inputdir, and
# Cloudberry's tests read theirs from abs_srcdir too, which pg_regress sets to
# --inputdir: its data files, where PostgreSQL's has none of the name.
mkdir -p "$WORK/data"
cp -r "$PGSUITE/data/." "$WORK/data/"
cp -rn "$CB/data/." "$WORK/data/"

# A group's suite: PostgreSQL 19's test_setup, the port's setup, then its
# tests.  Named as Cloudberry's own suite directory is: its tests print the
# paths of their data, which their matchsubs make /ABSPATH/src/test/regress/...
# of whatever comes before.
make_suite() {
	local g="$1" SN="$WORK/$1/src/test/regress" i t f src am aoseg suffix a s e
	mkdir -p "$SN/sql" "$SN/expected" "$WORK/$g/testtablespace"
	cp "$PGSUITE/sql/test_setup.sql" "$SN/sql/"
	cp "$PGSUITE/expected/test_setup.out" "$SN/expected/"
	cp -r "$WORK/data" "$SN/data"
	cp "$HERE"/sql/*.sql "$SN/sql/"
	cp "$HERE"/expected/*.out "$SN/expected/" 2> /dev/null
	if [ "$SUITE_DIR" != "$HERE" ]; then
		cp "$SUITE_DIR"/sql/*.sql "$SN/sql/" 2> /dev/null
		cp "$SUITE_DIR"/expected/*.out "$SN/expected/" 2> /dev/null
	fi
	# a test loads regress.so from PG_ABS_SRCDIR too, the suite's directory
	ln -sf "$("$BINDIR/pg_config" --pkglibdir)/cb_regress.so" "$SN/regress.so"
	# a schedule a pass, of the tests that run in it
	for p in planner orca; do
		for t in $SETUP; do echo "test: $t"; done > "$SN/schedule.$p"
	done
	for i in "${!run_tests[@]}"; do
		[ "${run_group[$i]}" = "$g" ] || continue
		t="${run_tests[$i]}"
		case "$t" in
			port:*)
				schedule_add "$SN" "${t#port:}" "${run_pass[$i]}"
				continue ;;
		esac
		# A test in a directory, and one Cloudberry's pg_regress makes twice,
		# _row and _column, from one file, are made as the singlenode suite
		# makes them: uao_dml/uao_dml_select_row runs as
		# uao_dml_uao_dml_select_row.
		f=$(echo "$t" | tr / _)
		src=$t am=
		for v in row:ao_row:aoseg column:ao_column:aocsseg; do
			IFS=: read -r suffix a s <<< "$v"
			if [ "${t%_$suffix}" != "$t" ] && [ -f "$CB/input/$(dirname "$t")/GENERATE_ROW_AND_COLUMN_FILES" ]; then
				src=${t%_$suffix} am=$a aoseg=$s
			fi
		done
		if [ -f "$CB/input/$src.source" ]; then
			convert "$g" "$CB/input/$src.source" | amsub | respell |
				copy_data_end > "$SN/sql/$f.sql"
		else
			convert "$g" "$CB/sql/$t.sql" | respell | copy_data_end > "$SN/sql/$f.sql"
		fi
		if [ -f "$CB/output/$src.source" ]; then
			convert "$g" "$CB/output/$src.source" | amsub | respell > "$SN/expected/$f.out"
		else
			for e in "$CB/expected/$t.out" "$CB"/expected/"$t"_[0-9].out; do
				[ -f "$e" ] || continue
				convert "$g" "$e" | respell \
					> "$SN/expected/$(echo "${e#"$CB"/expected/}" | tr / _)"
			done
		fi
		include_files "$g" "$SN" "$f"
		schedule_add "$SN" "$f" "${run_pass[$i]}"
	done
}
# include_files <group> <suite dir> <test>: a file the test includes, \i
# sql/<name>.sql, which psql reads from the directory the tests run in,
# Cloudberry's, converted and respelled as the test is, into the group's
# suite, and named there -- in the test and in its expected output, where
# psql echoes the line (the qp_with_functional tests').
include_files() {
	local g="$1" SN="$2" f="$3" inc e
	for inc in $(sed -n 's#^\\i \(sql/[A-Za-z0-9_./-]*\.sql\)$#\1#p' "$SN/sql/$f.sql" | sort -u); do
		[ -f "$CB/$inc" ] || continue
		mkdir -p "$SN/include/$(dirname "$inc")"
		convert "$g" "$CB/$inc" | respell > "$SN/include/$inc"
		for e in "$SN/sql/$f.sql" "$SN/expected/$f.out" "$SN"/expected/"$f"_[0-9].out; do
			[ -f "$e" ] && sed -i "s#^\\\\i $inc\$#\\\\i $SN/include/$inc#" "$e"
		done
	done
}

# schedule_add <suite dir> <test> <pass or ->: the test into the schedule of
# each pass it runs in.
schedule_add() {
	local p
	for p in planner orca; do
		if [ "$3" = - ] || [ "$3" = "$p" ]; then
			echo "test: $2" >> "$1/schedule.$p"
		fi
	done
}
# psql 19 skips the in-line data of a COPY ... FROM STDIN that fails, up to
# the next \. (PostgreSQL's d6ab88d374a), where psql 16, which Cloudberry's
# tests were written for, went on with the next line.  A COPY FROM STDIN a
# test expects to fail, with no data after it -- the next line a comment or
# a statement -- is given data that ends at once, which psql reads and does
# not echo, so that what the test runs next is run, as the singlenode suite
# gives it (../singlenode/run.sh).
copy_data_end() {
	awk '{
		l = tolower($0)
		# the blank lines after such a COPY, until what follows them is known
		if (pending && l ~ /^[ \t]*$/) {
			blanks = blanks $0 "\n"
			next
		}
		if (pending && (l ~ /^--/ || l ~ /^[ \t]*(abort|alter|analyze|begin|call|checkpoint|close|cluster|comment|commit|copy|create|deallocate|declare|delete|discard|do|drop|end|execute|explain|fetch|grant|insert|listen|lock|merge|notify|prepare|refresh|reindex|release|reset|revoke|rollback|savepoint|select|set|show|start|table|truncate|update|vacuum|values|with)([ \t;(]|$)/))
			print "\\."
		printf "%s", blanks
		blanks = ""
		# (not a line of a combined query of psql, ended by a backslash and a semicolon)
		pending = (l ~ /^[ \t]*copy[ \t].*[ \t]from[ \t]+stdin([ \t].*)?;[ \t]*(--.*)?$/ &&
				   l !~ /\\;[ \t]*(--.*)?$/)
		print
	}
	END { printf "%s", blanks }'
}
amsub() {
	if [ -n "$am" ]; then
		sed -e "s/@amname@/$am/g" -e "s/@aoseg@/$aoseg/g"
	else
		cat
	fi
}
for g in "${groups[@]}"; do
	make_suite "$g"
done
echo "  the clusters made in $((t1 - t0)) s, the groups' tests converted in $(( $(date +%s) - t1 )) s"
echo

mkdir -p "$WORK/gpdiff"
cp "$GPDIFF"/gpdiff.pl "$GPDIFF"/atmsort.pm "$GPDIFF"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$GPDIFF/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, for PostgreSQL's test_setup too, whose
# tables are distributed here and say so; with a difference reviewed and
# kept counted as none (canon.pl, cloudberry/, as the singlenode suite does).
# gpMgmt's gpconfig, which shows a setting (resource_group_gucs): asking the
# tests' database, which has gp_core, where it would ask template1, which
# here has none of the port's extensions -- a database a test makes from it
# has none either (external_table's).
cat > "$EXEC/bin/gpconfig" <<EOF
#!/bin/bash
PGDATABASE="\${PGDATABASE:-regression}" exec "$GPHOME/bin/gpconfig" "\$@"
EOF
chmod +x "$EXEC/bin/gpconfig"
# Cloudberry's client, through this: it sets gp_log_resqueue_memory by
# Cloudberry's name, which a compiled program keeps and respelling cannot
# reach, so the setting is given it by the port's as it connects.
if [ -x "$BINDIR/extended_protocol_resqueue" ]; then
	cat > "$EXEC/bin/extended_protocol_resqueue" <<EOF
#!/bin/bash
PGOPTIONS="\${PGOPTIONS:-} -c gp.log_resqueue_memory=on" exec "$BINDIR/extended_protocol_resqueue" "\$@"
EOF
	chmod +x "$EXEC/bin/extended_protocol_resqueue"
fi
# Cloudberry's mem_quota_util.py, which runs a query in many sessions at once
# through psql, as its resource queue lets them: it imports two modules of
# gppylib, Cloudberry's management utilities, that it does not use, which a
# package of that name, empty, stands for.
mkdir -p "$WORK/pylib/gppylib/commands"
touch "$WORK/pylib/gppylib/__init__.py" "$WORK/pylib/gppylib/gplog.py" \
	"$WORK/pylib/gppylib/commands/__init__.py" "$WORK/pylib/gppylib/commands/unix.py"
cat > "$EXEC/bin/mem_quota_util.py" <<EOF
#!/bin/bash
PYTHONPATH="$WORK/pylib\${PYTHONPATH:+:\$PYTHONPATH}" exec python3 "$CB/mem_quota_util.py" "\$@"
EOF
chmod +x "$EXEC/bin/mem_quota_util.py"
# Cloudberry's init files, the suite's and the port's: its matchsubs and
# matchignores.
inits=("$CB/init_file" "$HERE/init_file")
[ "$GPDIFF" = "$CB" ] || inits=("$GPDIFF/init_file" "${inits[@]}")
[ "$SUITE_DIR" = "$HERE" ] || [ ! -f "$SUITE_DIR/init_file" ] || inits+=("$SUITE_DIR/init_file")
# CB_DIFF_DIR is the group's pass's directory, which keeps what differs;
# CB_DIFF_MODE the pass, which names a difference kept for it alone.
cat > "$EXEC/bin/diff" <<EOF
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
    $(for f in "${inits[@]}"; do printf -- '--gpd_init "%s" ' "$f"; done))
canon="\$CB_DIFF_DIR/canon/\$name.diff"
# the results less the place PostgreSQL 19 gives a shell type (shellpos.pl)
mkdir -p "\$CB_DIFF_DIR/shellpos"
perl "$HERE/../singlenode/shellpos.pl" < "\$res" > "\$CB_DIFF_DIR/shellpos/\$(basename "\$res")"
res="\$CB_DIFF_DIR/shellpos/\$(basename "\$res")"
env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U0 "\${cb[@]}" "\$exp" "\$res" 2> /dev/null |
	perl "$HERE/../singlenode/canon.pl" > "\$canon"
# A comparison that could not be made is a difference, never an empty one.
st=("\${PIPESTATUS[@]}")
[ "\${st[0]}" -le 1 ] && [ "\${st[1]}" -eq 0 ] || echo "no comparison was made" >> "\$canon"
# Kept in the suite's cloudberry/ -- or, for a suite of another directory's,
# in this one's: the same test answers alike where its copy is Cloudberry's.
if [ ! -s "\$canon" ] || cmp -s "\$canon" "$SUITE_DIR/cloudberry/\$name.\$CB_DIFF_MODE.diff" ||
   cmp -s "\$canon" "$SUITE_DIR/cloudberry/\$name.diff" ||
   cmp -s "\$canon" "$HERE/cloudberry/\$name.\$CB_DIFF_MODE.diff" ||
   cmp -s "\$canon" "$HERE/cloudberry/\$name.diff"; then
	rm -f "\$canon"
	exit 0
fi
exec env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" "\${opts[@]}" "\${cb[@]}" "\$exp" "\$res"
EOF
chmod +x "$EXEC/bin/diff"

# A statement that runs for minutes is a finding, as the singlenode suite
# says; so is one that waits for ever on a lock a segment holds.  The
# watchdog does not look while a test runs that counts the coordinator's
# sessions, or the statements holding its slots of query metrics, where its
# own would be counted: the test whose results file is the newest.
TIMEOUT="${STATEMENT_TIMEOUT:-60 seconds}"
QUIET_TESTS=" instr_in_shmem instr_in_shmem_verify "
watchdog() {
	local pid query newest
	while :; do
		sleep 5
		newest=$(ls -t "$2/results" 2> /dev/null | head -1)
		case "$QUIET_TESTS" in *" ${newest%.out} "*) continue ;; esac
		PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -F ' ' -d postgres -c "
			SELECT pid, regexp_replace(left(query, 300), '\\s+', ' ', 'g')
			  FROM pg_stat_activity
			 WHERE datname = 'regression' AND state = 'active'
			   AND now() - query_start > interval '$TIMEOUT'" 2> /dev/null |
		while read -r pid query; do
			[ -n "$pid" ] || continue
			PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -d postgres \
				-c "SELECT pg_cancel_backend($pid)" > /dev/null 2>&1 &&
				echo "$(date +%T)  $query" >> "$1"
		done
	done
}

# One group's tests, in one pass, on its cluster: pg_regress's report in
# R/pg_regress.out, and its status in R/rc.
run_group() {
	local g="$1" gi="$2" pass="$3" optimizer="$4" wd rc
	local R="$WORK/$g/$pass" SN="$WORK/$g/src/test/regress" t0
	export PGHOST="$(node_sock "$g" 0)" PGPORT="$(node_port "$gi" 0)"
	mkdir -p "$R/canon"
	t0=$(date +%s)
	# Cloudberry's tests make functions from the regress.so of the directory
	# they run from, whose test functions of Cloudberry's own the port's
	# cb_regress.so serves (auth_constraint's check_auth_time_constraints)
	ln -sf "$("$BINDIR/pg_config" --pkglibdir)/cb_regress.so" "$R/regress.so"
	# and the tablespaces' directories Cloudberry's GNUmakefile makes beside
	# the results, PG_ABS_BUILDDIR, which its tests name
	(cd "$R" && mkdir -p testtablespace testtablespace_otherloc testtablespace_unlogged \
		testtablespace_default_tablespace testtablespace_temp_tablespace \
		testtablespace_mytempsp0 testtablespace_mytempsp1 testtablespace_mytempsp2 \
		testtablespace_mytempsp3 testtablespace_mytempsp4 testtablespace_database_tablespace \
		testtablespace_1111111111222222222233333333334444444444555555555566666666667777777777888888888899999999990000000000 \
		$(for i in 1 2 3 4 5 6 7 8; do echo testtablespace_existing_version_dir/$i/GPDB_99_399999991; done))
	# which Cloudberry's PG_ABS_SRCDIR has too, being the same directory: a
	# test lists and empties them there (gp_tablespace)
	for d in testtablespace testtablespace_existing_version_dir; do
		ln -sfn "$R/$d" "$SN/$d"
	done
	# ORCA's counts, from the pass's start (gp_orca.fallbacks()), and where
	# the coordinator's log was then.  template1 has gp_orca's functions only
	# once gp_setup, the pass's first test, has made them there, so they are
	# made here first -- without, the reset failed and the counts kept what
	# fell back before the pass, gp_core's recovery process's first query in
	# each group, which the log's tail does not have (3,898 counted, 3,890
	# in the log, in M7's recorded run).  gp_setup's IF NOT EXISTS passes
	# over it quietly.
	"$PSQL" -X -q -d template1 -c "SET gp.optimizer = off" \
		-c "SET client_min_messages = warning" \
		-c "CREATE EXTENSION IF NOT EXISTS gp_orca CASCADE" \
		-c "SELECT gp_orca.reset_fallbacks()" > /dev/null 2>&1
	logpos=$(stat -c %s "$(node_dir "$g" 0).log")
	watchdog "$R/cancelled" "$R" &
	wd=$!
	trap 'kill "$wd" 2> /dev/null; exit 1' TERM INT
	# From Cloudberry's suite's directory, as its Makefile runs it: its tests
	# read data/ by relative paths.  Its entries, that is, from a directory of
	# the group's, whose results/ is the pass's, as the Makefile's results/
	# is beside them: rowhints writes a plan there that sql/maskout.sh reads
	# back.  The suite's own directory is not the tests' to write in: data/
	# is the group's too, of links to Cloudberry's files, where gpsd and
	# minirepro write the dumps they read back.
	if [ ! -d "$WORK/$g/cwd" ]; then
		mkdir -p "$WORK/$g/cwd"
		for e in "$CB"/*; do
			case "$(basename "$e")" in
				results) ;;
				data) mkdir "$WORK/$g/cwd/data" && ln -s "$e"/* "$WORK/$g/cwd/data/" ;;
				*) ln -s "$e" "$WORK/$g/cwd/" ;;
			esac
		done
	fi
	ln -sfn "$R/results" "$WORK/$g/cwd/results"
	cd "$WORK/$g/cwd"
	# PG_HOSTNAME and PG_BINDDIR are what Cloudberry's pg_regress sets for
	# its tests: the host of segment 0, for their file:// and gpfdist://
	# locations -- here every node's is this one -- and the directory of
	# the programs, where they start gpfdist from; and PG_CURUSERNAME, the
	# user the tests are run as (gp_tablespace).  The coordinator's data
	# directory is named by both of its names: MASTER_DATA_DIRECTORY, the
	# older, is gp_dispatch_keepalives'.
	PATH="$EXEC/bin:$PATH" CB_DIFF_MODE="$pass" CB_DIFF_DIR="$R" \
	PGOPTIONS="-c gp.optimizer=$optimizer" \
	PG_HOSTNAME=localhost PG_BINDDIR="$BINDIR" PG_CURUSERNAME="$PGUSER" \
	PG_BINDIR="$BINDIR" COORDINATOR_DATA_DIRECTORY="$(node_dir "$g" 0)" \
	MASTER_DATA_DIRECTORY="$(node_dir "$g" 0)" \
		"$PG_REGRESS" \
			--bindir="$BINDIR" \
			--inputdir="$SN" \
			--expecteddir="$SN" \
			--outputdir="$R" \
			--dlpath="$PGSUITE" \
			--schedule="$SN/schedule.$pass" \
			--max-connections=20 \
			--host="$PGHOST" --port="$PGPORT" \
		> "$R/pg_regress.out" 2>&1
	rc=$?
	echo "$rc" > "$R/rc"
	echo $(( $(date +%s) - t0 )) > "$R/secs"
	kill "$wd" 2> /dev/null; wait "$wd" 2> /dev/null
	"$PSQL" -X -q -t -A -F ' ' -d template1 \
		-c "SELECT reason, count FROM gp_orca.fallbacks() WHERE reason IN ('planned', 'declined', 'error')" \
		> "$R/fallbacks" 2> /dev/null
	tail -c +"$((logpos + 1))" "$(node_dir "$g" 0).log" |
		sed -n 's/.*LOG:  ORCA fell back \(([a-z]*)\): \(Falling back to Postgres-based planner because GPORCA does not support the following feature: \)\{0,1\}\(.*\)$/\1: \3/p' \
		> "$R/fallback_reasons"

	# test_setup's tablespace outlives the database: gone before the next
	# pass.
	"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS regression" \
		-c "DROP TABLESPACE IF EXISTS regress_tblspace" > /dev/null 2>&1

	# What passed leaves no difference, even where one was written for
	# another expected output than the one it matched.
	sed -nE 's/^ok +[0-9]+ +[-+] +([^ ]+) .*/\1/p' "$R/pg_regress.out" |
	while read -r t; do
		rm -f "$R/canon/$t.diff" "$R/canon/$t"_[0-9].diff
	done
}

failed=0
for pass in ${PASSES:-planner orca}; do
	echo "== pass: $pass"
	case "$pass" in
		planner) optimizer=off ;;
		orca)    optimizer=on ;;
	esac
	for gi in "${!groups[@]}"; do
		run_group "${groups[$gi]}" "$gi" "$pass" "$optimizer" &
	done
	wait

	# In the manifest's order, as pg_regress reports each; test_setup and
	# gp_setup once, which pass when they pass in every group, in the
	# longest time they took.
	for g in "${groups[@]}"; do
		sed -nE 's/^(ok|not ok) +[0-9]+ +[-+] +([^ ]+) +([0-9]+) ms.*/\2 \1 \3/p' \
			"$WORK/$g/$pass/pg_regress.out" | sed 's/ not ok / not_ok /'
	done > "$WORK/$pass.results"
	total=0; bad=0; rc=0
	pass_tests=($SETUP)
	for i in "${!run_tests[@]}"; do
		[ "${run_pass[$i]}" = - ] || [ "${run_pass[$i]}" = "$pass" ] &&
			pass_tests+=("${run_tests[$i]}")
	done
	for t in "${pass_tests[@]}"; do
		t=$(echo "${t#port:}" | tr / _)
		read -r st ms < <(awk -v t="$t" '
			$1 == t { if ($2 != "ok") st = "not_ok"; else if (st == "") st = "ok"
			          if ($3 > ms) ms = $3 }
			END { print (st == "" ? "not_ok" : st), ms + 0 }' "$WORK/$pass.results")
		total=$((total + 1))
		[ "$st" = ok ] || bad=$((bad + 1))
		printf '  %-6s %-4d - %-44s %6d ms\n' "${st/_/ }" "$total" "$t" "$ms"
	done
	echo "  $((total - bad)) of $total passed"
	echo "  groups: $(for g in "${groups[@]}"; do printf '%s %s s, ' "$g" "$(cat "$WORK/$g/$pass/secs" 2> /dev/null)"; done | sed 's/, $//')"
	for g in "${groups[@]}"; do
		[ "$(cat "$WORK/$g/$pass/rc" 2> /dev/null)" = 0 ] || rc=1
	done
	if cat "$WORK"/*/"$pass"/cancelled 2> /dev/null | grep -q .; then
		echo "  cancelled after $TIMEOUT:"
		cat "$WORK"/*/"$pass"/cancelled | sed 's/^/    /'
	fi
	# What ORCA would not plan, as the numbers decision 1 keeps for M7's
	# decision of Route B: how many statements ORCA planned in the pass, how
	# many it was asked to and did not, and the reasons, most often first.
	if [ "$pass" = orca ]; then
		read -r planned declined errored < <(cat "$WORK"/*/orca/fallbacks 2> /dev/null |
			awk '{ n[$1] += $2 } END { print n["planned"] + 0, n["declined"] + 0, n["error"] + 0 }')
		echo "  ORCA planned $planned statements, and would not plan $((declined + errored)): $declined declined, $errored raised"
		cat "$WORK"/*/orca/fallback_reasons 2> /dev/null | sort | uniq -c | sort -rn > "$WORK/orca-fallbacks"
		head -12 "$WORK/orca-fallbacks" | sed 's/^/    /'
		[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/orca-fallbacks" "$RESULTS_DIR/greenplum-orca-fallbacks.txt"
	fi
	unreviewed=$(for g in "${groups[@]}"; do
					 (cd "$WORK/$g/$pass/canon" && ls -- *.diff 2> /dev/null | sed 's/\.diff$//')
				 done)
	if [ -n "$unreviewed" ]; then
		echo "  differences no one has reviewed, from the expected output named:"
		echo $unreviewed | fold -s -w 72 | sed 's/^/    /'
	fi
	if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
		failed=$((failed + 1))
		if [ -n "${RESULTS_DIR:-}" ]; then
			mkdir -p "$RESULTS_DIR/greenplum-$pass-canon" "$RESULTS_DIR/greenplum-$pass-results"
			for g in "${groups[@]}"; do
				cat "$WORK/$g/$pass/regression.diffs" >> "$RESULTS_DIR/greenplum-$pass.diffs" 2> /dev/null
				cp "$WORK/$g/$pass"/canon/*.diff "$RESULTS_DIR/greenplum-$pass-canon/" 2> /dev/null
				cp -r "$WORK/$g/$pass"/results/. "$RESULTS_DIR/greenplum-$pass-results/" 2> /dev/null
				cp "$WORK/$g/$pass/pg_regress.out" "$RESULTS_DIR/greenplum-$pass-$g.out" 2> /dev/null
			done
		fi
	fi
	echo
done

[ "$failed" -eq 0 ]
