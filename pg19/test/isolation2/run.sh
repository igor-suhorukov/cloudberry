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
# Part of Cloudberry's isolation2_schedule, on a cluster: M3's tests --
# distributed transactions, snapshots, locks and the global deadlock
# detector -- M4's, FTS and mirrors, M6's resource queues and memory
# accounting, and M7's of the tools: a node recovered elsewhere, and the
# standby coordinator promoted, made again and waited for.  And
# parallel_retrieve_cursor_schedule, M8's: parallel retrieve cursors and the
# retrieve sessions that read them (1R:, *R:).
#
# src/test/isolation2 is Cloudberry's suite of tests that need more than one
# session at a time, written in its isolation2 syntax (1: ..., 2&: ..., 2<:,
# 0U: for a session of segment 0's own) and run by its own driver,
# sql_isolation_testcase.py, over PyGreSQL, against its demo cluster.  The
# port runs part of it the same way, on a coordinator and three segments of
# its own, as the greenplum suite makes them: manifest says of each test of
# the schedule it names whether it runs and, if not, why.  The driver is run
# as Cloudberry's pg_isolation2_regress runs it, but for one change to a copy
# of it: a session of a segment's own is opened without "-c gp_role=utility",
# which PostgreSQL 19 has no such setting for, and which a connection to a
# segment is here anyway.  Cloudberry's settings are respelled as the port
# spells them, as the greenplum suite does, in the tests and in their
# expected output alike.  The setup is setup.sql here, in place of
# Cloudberry's, whose helpers are PL/Python the port's cluster does not need:
# its pg_ctl() restarts a node with COPY ... TO PROGRAM.
#
# Each test is compared as Cloudberry's pg_regress compares it: gpdiff.pl
# under Cloudberry's init files and the port's, or exactly a difference in
# cloudberry/, reviewed and kept (see ../singlenode/canon.pl for the form).
# Two passes, as the other suites have: the planner and ORCA.
#
# The tests run in the groups the manifest puts them in, each group on a
# cluster of its own and in the schedule's order, the groups of a pass side
# by side; a test the manifest puts in several groups passes when it passes
# in each.  What a pass reports is in the schedule's order, whichever group
# finished first.  The group named standby has a standby coordinator, as
# Cloudberry's demo cluster has, made with pg_basebackup; and a group whose
# name begins "mirrors" has that cluster whole, as Cloudberry's FTS tests
# assume it: a mirror for each segment too, a hot standby streaming from its
# primary, and FTS on the coordinator (M4).  Shell commands of the tests --
# and the nodes themselves, which a test's PL/Python helper runs one from --
# find gpMgmt's gpconfig, gpstop, gprecoverseg and gpinitstandby (M7), set up
# as ../gpmgmt/tools.sh sets them up, gpfts in bin/ here, and
# COORDINATOR_DATA_DIRECTORY; and "-c gp_role=utility" goes from them as it
# goes from the driver's sessions of a node's own.  The tools connect to
# template1, which has gp_core too, so a test's database is made from
# template0, as it was made before template1 had it.  A group's nodes share
# one socket directory, their host in gp_segment_configuration, which a node
# the tools copy -- a mirror recovered, a standby made -- has from the node
# it is a copy of.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Another schedule of Cloudberry's suite runs through this harness too, with
# a manifest and reviewed differences of its own: ../resgroup's, the
# resource group tests (ISOLATION2_SUITE names it in what is printed).
SUITE="${ISOLATION2_SUITE:-isolation2}"
MANIFEST="${ISOLATION2_MANIFEST:-$HERE/manifest}"
KEPT="${ISOLATION2_KEPT:-$HERE/cloudberry}"
SCHEDULE_NAME="${ISOLATION2_SCHEDULE_NAME:-isolation2_schedule}"
# ... and the database it runs in, and the init file of Cloudberry's that its
# target in Cloudberry's Makefile reads, with the port's own beside the
# isolation2 suite's
DBNAME="${ISOLATION2_DBNAME:-isolation2test}"
EXTRA_INIT="${ISOLATION2_EXTRA_INIT:-}"
# ... and rules of sed of the schedule's own, applied as the port's
# respelling is, to each test and its expected output alike (../resgroup's
# parent of the cgroups)
EXTRA_SED="${ISOLATION2_EXTRA_SED:-}"
CB="${CB_ISOLATION2_DIR:-/cb/src/test/isolation2}"
CB_INIT="${ISOLATION2_CB_INIT:-$CB/init_file_isolation2}"
GPDIFF="${GPDIFF_DIR:-/cb/src/test/regress}"

if [ ! -f "$CB/sql_isolation_testcase.py" ] || [ ! -f "$GPDIFF/gpdiff.pl" ] ||
   [ ! -f "$GPDIFF/init_file" ] || ! python3 -c 'import pg' 2> /dev/null; then
	echo "Cloudberry's isolation2 suite, gpdiff.pl or PyGreSQL is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-isolation2c-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbi2-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-isolation2c-XXXXXX")"
if ! . "$HERE/../gpmgmt/tools.sh" "$EXEC"; then
	echo "gpMgmt, or the Python it needs, is not installed; skipping"
	rm -rf "$WORK" "$SOCK" "$EXEC"
	exit 77
fi
BASEPORT="${PGPORT:-$((7500 + RANDOM % 200))}"
NODES=4					# a coordinator and Cloudberry's three segments
PRELOAD='gp_core,gp_orca,gp_sql,gp_resource'
# The superuser is Cloudberry's demo cluster's, gpadmin, as the greenplum
# suite's is: Cloudberry's expected output names it, and a PL/Python helper
# of a test that runs psql -- in the server's environment, which the nodes
# are started in -- connects as it.
export PGUSER=gpadmin
SECRET="isolation2-schedule-$RANDOM$RANDOM$RANDOM"

# The tests the manifest runs, in its order, and the group each is in; the
# groups, in the order they first appear.  A group name ending in "*" is
# every group that name begins.
run_tests=(); run_group=()
while read -r t g; do
	run_tests+=("$t"); run_group+=("${g:-main}")
done < <(awk '$1 == "run" { print $2, $3 }' "$MANIFEST")
groups=()
for g in "${run_group[@]}"; do
	[[ "$g" == *"*" ]] && continue
	[[ " ${groups[*]} " == *" $g "* ]] || groups+=("$g")
done
in_group() {					# in_group <test's group> <group>
	[ "$1" = "$2" ] || { [[ "$1" == *"*" ]] && [[ "$2" == "${1%\*}"* ]]; }
}
for i in "${!run_tests[@]}"; do
	found=0
	for g in "${groups[@]}"; do
		in_group "${run_group[$i]}" "$g" && found=1
	done
	[ "$found" -eq 1 ] || { echo "manifest: ${run_tests[$i]} is in no group (${run_group[$i]})"; exit 1; }
done

# Node n of the cluster of the gi-th group, and a group's standby; and the
# socket directory every node of a group is reached by, its host.
node_dir()  { echo "$WORK/$1/node$2"; }
node_port() { echo $((BASEPORT + $1 * NODES + $2)); }
group_sock() { echo "$SOCK/$1"; }
standby_port() { echo $((BASEPORT + 100 + $1)); }
has_mirrors() { [[ "$1" == mirrors* ]]; }
has_standby() { [ "$1" = standby ] || has_mirrors "$1"; }
# Content c's mirror in the gi-th group, and the standby's dbid: Cloudberry's
# demo cluster's, the mirrors 5..7 and the standby 8, where there are mirrors.
mirror_dir()  { echo "$WORK/$1/mirror$2"; }
mirror_port() { echo $((BASEPORT + 300 + $1 * NODES + $2)); }
# ... and the k-th node a test adds with gpexpand, as Cloudberry's demo
# cluster's port 7008 + k (own_nodes)
new_node_port() { echo $((BASEPORT + 400 + $1 * NODES + $2)); }
standby_dbid() { if has_mirrors "$1"; then echo $((2 * NODES)); else echo $((NODES + 1)); fi; }

cleanup() {
	for g in "${groups[@]}"; do
		for n in $(seq 0 $((NODES - 1))) standby $(seq -f 'mirror%g' 0 $((NODES - 2))); do
			case "$n" in
				standby) d="$WORK/$g/standby" ;;
				mirror*) d="$WORK/$g/$n" ;;
				*) d="$(node_dir "$g" "$n")" ;;
			esac
			[ -d "$d" ] || continue
			# ... and the log of a start of gpstart's, in log/startup.log
			if [ -n "${RESULTS_DIR:-}" ]; then
				cp "$d.log" "$RESULTS_DIR/$SUITE-$g-node$n.log" 2> /dev/null
				cp "$d/log/startup.log" "$RESULTS_DIR/$SUITE-$g-node$n.startup.log" 2> /dev/null
			fi
			"$BINDIR/pg_ctl" -D "$d" -m immediate stop > /dev/null 2>&1
		done
	done
	# ... and the segments a test added with gpexpand, which a test that
	# failed leaves running (own_nodes)
	for d in "$WORK"/*/*/datadirs/*/*; do
		[ -f "$d/postmaster.pid" ] && "$BINDIR/pg_ctl" -D "$d" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

echo "$SUITE: part of Cloudberry's $SCHEDULE_NAME, on a coordinator and three segments"
printf '  of the %d tests of the schedule the manifest lists: %d run here, in %d groups, %d are skipped\n' \
	"$(awk '$1 == "run" || $1 == "skip"' "$MANIFEST" | wc -l)" \
	"${#run_tests[@]}" "${#groups[@]}" \
	"$(awk '$1 == "skip"' "$MANIFEST" | wc -l)"
awk '$1 == "skip" { $1 = ""; sub(/^ /, ""); print "  skip " $0 }' "$MANIFEST" | cut -c1-150
echo

# A group's cluster, as the greenplum suite makes one.  Every node's log is
# the file beside its data directory, which pg_ctl() in setup.sql relies on.
make_cluster() {
	local g="$1" gi="$2" conf="$WORK/$1/gp_cluster.conf" n

	# What a shell command of a test is given (run_group), every node is
	# started with, so that what a node runs itself -- pg_ctl() here, a PL/Python
	# helper's gpinitstandby -- finds the tools and the coordinator too, as the
	# servers of Cloudberry's demo cluster are started from its environment.
	export PG_BINDIR="$BINDIR"
	export PGHOST="$(group_sock "$g")" PGPORT="$(node_port "$gi" 0)"
	export COORDINATOR_DATA_DIRECTORY="$(node_dir "$g" 0)"

	mkdir -p "$WORK/$g" "$(group_sock "$g")"
	{
		echo "# dbid content role host port datadir"
		for n in $(seq 0 $((NODES - 1))); do
			echo "$((n + 1)) $((n - 1)) p $(group_sock "$g") $(node_port "$gi" "$n") $(node_dir "$g" "$n")"
		done
		if has_mirrors "$g"; then
			for n in $(seq 0 $((NODES - 2))); do
				echo "$((NODES + 1 + n)) $n m $(group_sock "$g") $(mirror_port "$gi" "$n") $(mirror_dir "$g" "$n")"
			done
		fi
		has_standby "$g" &&
			echo "$(standby_dbid "$g") -1 m $(group_sock "$g") $(standby_port "$gi") $WORK/$g/standby"
	} > "$conf"
	for n in $(seq 0 $((NODES - 1))); do
		"$BINDIR/initdb" -D "$(node_dir "$g" "$n")" -N -U gpadmin --locale=C --encoding=UTF8 \
			> "$WORK/$g/initdb$n.log" 2>&1 \
			|| { echo "initdb failed for node $n of group $g"; tail -20 "$WORK/$g/initdb$n.log"; return 1; }
		{
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "unix_socket_directories = '$(group_sock "$g")'"
			echo "listen_addresses = ''"
			echo "port = $(node_port "$gi" "$n")"
			echo "fsync = off"
			echo "gp.cluster_config = '$conf'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			# as Cloudberry's demo cluster, which prepare_limit says first
			echo "max_prepared_transactions = 250"
			# and its postgresql.conf.sample: a statement's memory is its
			# resource queue's to give
			echo "gp.resqueue_memory_policy = 'eager_free'"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		} >> "$(node_dir "$g" "$n")/postgresql.auto.conf"
	done
	for n in $(seq 1 $((NODES - 1))) 0; do
		# The mirrors before the coordinator, as gpinitsystem makes them, so
		# that FTS's first probe finds them there.
		[ "$n" -eq 0 ] && has_mirrors "$g" && { make_mirrors "$g" "$gi" || return 1; }
		"$BINDIR/pg_ctl" -D "$(node_dir "$g" "$n")" -l "$(node_dir "$g" "$n").log" -w -t 60 start \
			> /dev/null 2>&1 \
			|| { echo "node $n of group $g did not start"; tail -20 "$(node_dir "$g" "$n").log"; return 1; }
	done
	# The driver asks database postgres which nodes there are, when a test
	# runs a shell command with one's address, and some tests run there: it
	# has gp_core too, and so its gp_segment_configuration; and gp_resource,
	# whose procedures a resource group test's helper, running psql there,
	# makes a group with.  gpMgmt's tools ask template1, which has gp_core.
	"$PSQL" -X -q -d postgres -c "CREATE EXTENSION gp_core" \
		-c "CREATE EXTENSION gp_resource" > /dev/null 2>&1
	"$PSQL" -X -q -d template1 -c "CREATE EXTENSION gp_core" > /dev/null 2>&1

	# A standby coordinator: the coordinator's copy, streaming from it as
	# gp_walreceiver, as Cloudberry's standby does, so that the coordinator's
	# commits wait for it (gp_standby.c).  No hot standby in group standby,
	# as Cloudberry's hot_standby is off by default -- which lets a test lower
	# max_prepared_transactions on every node, as prepare_limit does, where a
	# hot standby refuses one below its primary's; a hot standby in a group
	# with mirrors, as its mirrors are, since a test there sets faults on it
	# over a connection of its own (commit_blocking_on_standby), where
	# Cloudberry's fault injector reaches a standby that takes none.
	if has_standby "$g"; then
		"$BINDIR/pg_basebackup" -D "$WORK/$g/standby" -h "$(group_sock "$g")" \
			-p "$(node_port "$gi" 0)" -X stream -c fast > "$WORK/$g/basebackup.log" 2>&1 \
			|| { echo "the standby of group $g could not be copied"; tail -5 "$WORK/$g/basebackup.log"; return 1; }
		{
			echo "port = $(standby_port "$gi")"
			echo "gp.dbid = $(standby_dbid "$g")"
			echo "hot_standby = $(has_mirrors "$g" && echo on || echo off)"
			echo "primary_conninfo = 'host=$(group_sock "$g") port=$(node_port "$gi" 0) application_name=gp_walreceiver'"
		} >> "$WORK/$g/standby/postgresql.auto.conf"
		touch "$WORK/$g/standby/standby.signal"
		"$BINDIR/pg_ctl" -D "$WORK/$g/standby" -l "$WORK/$g/standby.log" -w -t 60 start \
			> /dev/null 2>&1 \
			|| { echo "the standby of group $g did not start"; tail -20 "$WORK/$g/standby.log"; return 1; }
	fi

	# The pairs in sync, and synchronous replication on, as the tests begin.
	if has_mirrors "$g"; then
		local synced=
		for _ in $(seq 300); do
			synced=$("$PSQL" -X -q -t -A -d postgres \
				-c "SELECT gp_request_fts_probe_scan(); SELECT count(*) FROM gp_segment_configuration WHERE content >= 0 AND mode <> 's'" \
				2> /dev/null | tail -1)
			[ "$synced" = 0 ] && break
			sleep 0.2
		done
		[ "$synced" = 0 ] || { echo "the mirrors of group $g did not come in sync"; return 1; }
	fi
	return 0
}

# Each segment's mirror: a copy of its primary made with pg_basebackup, and a
# hot standby streaming from it as gp_walreceiver from its slot, as
# Cloudberry's mirrors are named; its log beside its data directory, as every
# node's is.
make_mirrors() {
	local g="$1" gi="$2" c d
	for c in $(seq 0 $((NODES - 2))); do
		d="$(mirror_dir "$g" "$c")"
		"$BINDIR/pg_basebackup" -D "$d" -h "$(group_sock "$g")" -p "$(node_port "$gi" $((c + 1)))" \
			-X stream -c fast -C -S internal_wal_replication_slot > "$WORK/$g/basebackup-m$c.log" 2>&1 \
			|| { echo "the mirror of content $c of group $g could not be copied"; tail -5 "$WORK/$g/basebackup-m$c.log"; return 1; }
		grep -v -E '^(port|gp\.dbid|primary_conninfo|primary_slot_name|hot_standby) ' \
			"$d/postgresql.auto.conf" > "$d.auto"
		{
			cat "$d.auto"
			echo "port = $(mirror_port "$gi" "$c")"
			echo "gp.dbid = $((NODES + 1 + c))"
			echo "hot_standby = on"
			echo "primary_conninfo = 'host=$(group_sock "$g") port=$(node_port "$gi" $((c + 1))) application_name=gp_walreceiver'"
			echo "primary_slot_name = 'internal_wal_replication_slot'"
		} > "$d/postgresql.auto.conf"
		rm -f "$d.auto"
		touch "$d/standby.signal"
		"$BINDIR/pg_ctl" -D "$d" -l "$d.log" -w -t 60 start > /dev/null 2>&1 \
			|| { echo "the mirror of content $c of group $g did not start"; tail -20 "$d.log"; return 1; }
	done
}
# gpfts, which a test runs as Cloudberry's external FTS, beside gpMgmt's
# tools and where every node finds them too: a test's PL/Python helper runs
# gpinitstandby in the server's environment (dtm_recovery_on_standby).
cp "$HERE"/bin/* "$EXEC/bin/"
chmod +x "$EXEC"/bin/*

for gi in "${!groups[@]}"; do
	make_cluster "${groups[$gi]}" "$gi" &
done
for g in "${groups[@]}"; do
	wait -n || exit 1
done

# The settings the port has, respelled as the greenplum suite respells them
# (../respell.pl); and, in the expected output, every word that names one,
# which is where a SHOW's column header has it too -- "gp_" and "gp." are the
# same length.  Beside SET, RESET, SHOW, current_setting() and set_config():
# ALTER SYSTEM; ... FROM pg_settings WHERE name = 'gp_session_id';
# show_guc('gp_resource_group_cpu_limit'), the resource group tests'
# PL/Python helper, which SHOWs the setting it is named; and query LIKE
# '%gp_vmem_idle_resource_timeout%', a test finding the session that SET it
# by the statement it ran, which it ran respelled.
{
	echo "kinds setsys altersys func nameeq showguc like header0"
	PGHOST="$(group_sock "${groups[0]}")" PGPORT="$(node_port 0 0)" \
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
			repl_catchup_within_range|\
			enable_offload_entry_to_qe|debug_dtm_action*|debug_abort_after_distributed_prepared|\
			debug_print_full_dtm|enable_answer_query_using_materialized_views|aqumv_allow_foreign_table)
				cbname="$short" ;;
			*) cbname="gp_$short" ;;
		esac
		echo "map $cbname $name"
	done
	# ... and no utility mode but a node's own connection, in a shell command
	# too
	echo "sed s/-c gp_role=utility//g"
	# A backend waiting for a resource group's slot waits on the Extension
	# wait event ResourceGroup, where Cloudberry's wait event has a type of
	# its own, ResourceGroup: a test that finds the waiter by its type finds
	# it by name.
	echo "sed s/wait_event_type\\s*=\\s*'ResourceGroup'/wait_event='ResourceGroup'/g"
	# A resource group test's PL/Python helper calls the server's
	# get_tablespace_path() through ctypes, which takes a function's result
	# as a C int unless it is told otherwise: a pointer cut to 32 bits, which
	# on the port's server, a position-independent executable whose heap is
	# far above that, it then reads a string at.  It is told the function
	# returns one, in the helper and in the expected output, which echoes it
	# on one line.
	echo "sed s/(get_tablespace_path = postgres\\['get_tablespace_path'\\])/\\1; get_tablespace_path.restype = ctypes.c_void_p/"
	[ -n "$EXTRA_SED" ] && sed 's/^/sed /' "$EXTRA_SED"
} > "$WORK/respell"
respell() { perl "$HERE/../respell.pl" "$WORK/respell" "$@"; }

# The driver, less "-c gp_role=utility"; run from Cloudberry's directory, as
# it sources global_sh_executor.sh from there.  A session it opens while a
# node restarts after a crash it asks again, as it asks while the node says
# it is resetting or in recovery -- and also when the connection is closed
# before the node says anything: PostgreSQL 19's postmaster may end the
# process it started for a connection as it resets, where a test connects
# right after failing the coordinator (dtx_recovery_wait_lsn); and while
# the node is "not yet accepting connections", which PostgreSQL 19 says
# once redo has begun, in crash recovery too (PMSIGNAL_RECOVERY_STARTED),
# where the same test's connection may come on a busy machine.
sed -e 's/given_opt="-c gp_role=utility"/given_opt=None/' \
	-e 's/("the database system is starting up" in str(e) or/&\n                         ("server closed the connection unexpectedly" in str(e) and "failed:" in str(e)) or\n                         "the database system is not yet accepting connections" in str(e) or/' \
	"$CB/sql_isolation_testcase.py" > "$EXEC/sql_isolation_testcase.py"

# ... and its shell helpers, a copy whose masks of a value a test found --
# an endpoint's host, among the retrieve tests' (parse_endpoint_info()) --
# quote it: a node's host is a socket's directory here, whose slashes end
# the pattern Cloudberry's helper writes, and whose first character is no
# word's, where Cloudberry's pattern begins at a word's boundary.
cp "$CB/global_sh_executor.sh" "$EXEC/global_sh_executor.sh"
cat >> "$EXEC/global_sh_executor.sh" <<'HELPER'

create_match_sub_with_spaces() {
    to_replace=""
    for var in "$@"
        do
        if [ -z "$to_replace" ]
        then
            to_replace=$var
        else
            quoted="$(printf '%s' "$to_replace" | sed 's/[^[:alnum:]_]/\\&/g')"
            export MATCHSUBS="${MATCHSUBS}${NL}m/(?<!\\w)${quoted}(?!\\w)/${NL}s/(?<!\\w)${quoted} */${var} /${NL}"
            to_replace=""
        fi
    done
    echo "${RAW_STR}"
}
HELPER
sed -i "s#source global_sh_executor.sh#source $EXEC/global_sh_executor.sh#" \
	"$EXEC/sql_isolation_testcase.py"

mkdir -p "$WORK/gpdiff"
cp "$GPDIFF"/gpdiff.pl "$GPDIFF"/atmsort.pm "$GPDIFF"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$GPDIFF/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

gpdiff() {
	env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" \
		-I HINT: -I CONTEXT: -I GP_IGNORE: "$@"
}

# A test's data file, @abs_srcdir@/data/..., is the regression suite's: its
# Makefile links data to src/test/regress/data.
convert() {
	sed -e "s#@abs_srcdir@/data/#$CB/../regress/data/#g" \
	    -e "s#@abs_srcdir@#$CB#g" \
	    -e "s#@abs_builddir@#$CB#g" \
	    -e "s#@testtablespace@#/tmp/testtablespace#g" \
	    -e "s#@bindir@#$BINDIR#g" \
	    -e "s#@libdir@#${PG_REGRESS_SUITE:-/cb/pgregress}#g" \
	    -e "s#@curusername@#$PGUSER#g" \
	    -e "s#@DLSUFFIX@#.so#g" "$1"
}

# The init files of the schedule a test is of, where it is not
# isolation2_schedule: parallel_retrieve_cursor_schedule's, which its target
# in Cloudberry's Makefile reads, and the port's for it -- the port's own
# test of it too.
schedule_inits() {
	case "$1" in
		parallel_retrieve_cursor/*|port/parallel_retrieve_cursor*)
			echo "--gpd_init $CB/init_file_parallel_retrieve_cursor --gpd_init $HERE/init_file_parallel_retrieve_cursor" ;;
	esac
}

# One group's tests, in one pass, on its cluster: a line "ok" or "bad" and
# the test in R/status for each, and what differs in R/regression.diffs.
run_group() {
	local g="$1" gi="$2" pass="$3" optimizer="$4"
	local R="$WORK/$g/$pass" t res exp name dir out i
	export PGHOST="$(group_sock "$g")" PGPORT="$(node_port "$gi" 0)"
	export PG_BINDIR="$BINDIR"
	export COORDINATOR_DATA_DIRECTORY="$(node_dir "$g" 0)"

	mkdir -p "$R/results" "$R/canon" "$R/sql" "$R/expected"
	own_tmp() { sed -E "s#/tmp/([A-Za-z0-9_]+)#$R/\\1#g"; }
	own_host() { sed -E "s#os\\.uname\\(\\)\\[1\\]#'$(group_sock "$g")'#g"; }
	own_nodes() {
		local k e=()
		for k in 0 1 2 3; do
			e+=(-e "s#localhost|localhost|70$(printf %02d $((8 + k)))|#$(group_sock "$g")|$(group_sock "$g")|$(new_node_port "$gi" "$k")|#g")
		done
		sed "${e[@]}"
	}
	: > "$R/status"
	"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DBNAME" > /dev/null 2>&1
	"$PSQL" -X -q -d postgres -c "CREATE DATABASE $DBNAME TEMPLATE template0" > /dev/null
	if ! out=$(sed -e "s#@BINDIR@#$BINDIR#g" "$HERE/setup.sql" |
			   "$PSQL" -X -q -v ON_ERROR_STOP=1 -d "$DBNAME" -f - 2>&1); then
		echo "  the setup failed in group $g:" > "$R/setup-failed"
		printf '%s\n' "$out" | sed 's/^/    /' >> "$R/setup-failed"
		return 1
	fi

	for i in "${!run_tests[@]}"; do
		in_group "${run_group[$i]}" "$g" || continue
		t="${run_tests[$i]}"
		res="$R/results/$t.out"
		mkdir -p "$(dirname "$res")" "$(dirname "$R/canon/$t")" \
			"$(dirname "$R/sql/$t")" "$(dirname "$R/expected/$t")"

		# What a test makes under /tmp is its pass's and group's own: the
		# two passes run side by side, as jobs of their own in a full run,
		# and two tablespaces cannot share a directory (mirror_promotion's),
		# nor two recoveries a file (recoverseg_from_file's, which one pass
		# would read the other's recovery from, and remove).  And the name
		# a test gives this machine, os.uname()[1], is its nodes' host: the
		# socket directory they share, by which, with its port and data
		# directory, gprecoverseg -i finds a node (recoverseg_from_file's).
		# So is a node gpexpand adds, localhost at Cloudberry's port 7008
		# and after in its input file: the group's socket directory, at a
		# port of the group's (gpexpand_gpshrink's).  A test whose name begins "port/" is the port's own, from sql/port
		# and expected/port beside this file, as Cloudberry's are from its
		# suite's.
		src="$CB"
		[[ "$t" == port/* ]] && src="$HERE"
		if [ -f "$src/input/$t.source" ]; then
			convert "$src/input/$t.source" | respell | own_tmp | own_host | own_nodes > "$R/sql/$t.sql"
		else
			respell "$src/sql/$t.sql" | own_tmp | own_host | own_nodes > "$R/sql/$t.sql"
		fi
		exp="$src/expected/$t.out"
		[ "$pass" = orca ] && [ -f "$src/expected/${t}_optimizer.out" ] && exp="$src/expected/${t}_optimizer.out"
		[ -f "$src/output/$t.source" ] && exp="$src/output/$t.source"
		name="$(basename "$exp" .out)"
		name="${name%.source}"
		convert "$exp" | respell | own_tmp | own_host | own_nodes > "$R/expected/$t.out"

		# As pg_isolation2_regress runs it, from the suite's directory; how
		# long it ran is reported with its result, as pg_regress reports it.
		t0=$(date +%s%N)
		( cd "$CB" && PGOPTIONS="-c gp.optimizer=$optimizer" \
			timeout 600 python3 "$EXEC/sql_isolation_testcase.py" \
				--dbname="$DBNAME" --initfile_prefix="$res" \
				< "$R/sql/$t.sql" > "$res" 2>&1 )
		ms=$(( ($(date +%s%N) - t0) / 1000000 ))

		# The port's first: what it takes off a segment's error -- which
		# segment -- leaves the lines Cloudberry's init files mask.
		inits=($(schedule_inits "$t") --gpd_init "$HERE/init_file" --gpd_init "$GPDIFF/init_file"
		       --gpd_init "$CB_INIT")
		[ -n "$EXTRA_INIT" ] && inits+=(--gpd_init "$EXTRA_INIT")
		[ -s "$res.initfile" ] && inits+=(--gpd_init "$res.initfile")

		gpdiff -U0 "${inits[@]}" "$R/expected/$t.out" "$res" 2> /dev/null |
			perl "$HERE/../singlenode/canon.pl" > "$R/canon/$t.diff"
		# A comparison that could not be made is a difference, never an empty one.
		st=("${PIPESTATUS[@]}")
		[ "${st[0]}" -le 1 ] && [ "${st[1]}" -eq 0 ] || echo "no comparison was made" >> "$R/canon/$t.diff"
		dir="$KEPT/$(dirname "$t")"
		if [ ! -s "$R/canon/$t.diff" ] ||
		   cmp -s "$R/canon/$t.diff" "$dir/$name.$pass.diff" ||
		   cmp -s "$R/canon/$t.diff" "$dir/$name.diff"; then
			rm -f "$R/canon/$t.diff"
			echo "ok $t $ms" >> "$R/status"
		else
			echo "bad $t $ms" >> "$R/status"
			gpdiff -U3 "${inits[@]}" "$R/expected/$t.out" "$res" >> "$R/regression.diffs" 2> /dev/null
		fi
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

	setup_failed=0
	for g in "${groups[@]}"; do
		if [ -f "$WORK/$g/$pass/setup-failed" ]; then
			cat "$WORK/$g/$pass/setup-failed"
			setup_failed=1
		fi
	done

	# In the schedule's order: a test passes when every group it ran in
	# says so; its time is the longest it took in one.
	total=0; bad=0
	for t in "${run_tests[@]}"; do
		total=$((total + 1))
		results=$(for g in "${groups[@]}"; do
					  awk -v t="$t" '$2 == t { print $1 }' "$WORK/$g/$pass/status" 2> /dev/null
				  done)
		ms=$(for g in "${groups[@]}"; do
				 awk -v t="$t" '$2 == t { print $3 }' "$WORK/$g/$pass/status" 2> /dev/null
			 done | sort -n | tail -1)
		if [ -n "$results" ] && ! grep -qv '^ok$' <<< "$results"; then
			printf '  ok     %-56s %7s ms\n' "$t" "${ms:-0}"
		else
			bad=$((bad + 1))
			printf '  NOT OK %-56s %7s ms\n' "$t" "${ms:-0}"
		fi
	done
	echo "  Cloudberry's tests: $((total - bad)) of $total passed"
	if [ "$bad" -ne 0 ] || [ "$setup_failed" -ne 0 ]; then
		failed=$((failed + 1))
		if [ -n "${RESULTS_DIR:-}" ]; then
			for g in "${groups[@]}"; do
				mkdir -p "$RESULTS_DIR/$SUITE-$pass/$g"
				cp -r "$WORK/$g/$pass/results" "$WORK/$g/$pass/canon" \
					"$RESULTS_DIR/$SUITE-$pass/$g/" 2> /dev/null
				cat "$WORK/$g/$pass/regression.diffs" >> "$RESULTS_DIR/$SUITE-$pass.diffs" 2> /dev/null
			done
		fi
	fi
	echo
done

[ "$failed" -eq 0 ]
