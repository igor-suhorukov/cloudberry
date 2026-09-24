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
# detector.
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
# finished first.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CB="${CB_ISOLATION2_DIR:-/cb/src/test/isolation2}"
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
BASEPORT="${PGPORT:-$((7500 + RANDOM % 200))}"
NODES=4					# a coordinator and Cloudberry's three segments
PRELOAD='gp_core,gp_orca,gp_sql'
SECRET="isolation2-schedule-$RANDOM$RANDOM$RANDOM"

# The tests the manifest runs, in its order, and the group each is in; the
# groups, in the order they first appear.  A group name ending in "*" is
# every group that name begins.
run_tests=(); run_group=()
while read -r t g; do
	run_tests+=("$t"); run_group+=("${g:-main}")
done < <(awk '$1 == "run" { print $2, $3 }' "$HERE/manifest")
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

# Node n of the cluster of the gi-th group.
node_dir()  { echo "$WORK/$1/node$2"; }
node_port() { echo $((BASEPORT + $1 * NODES + $2)); }
node_sock() { echo "$SOCK/$1/n$2"; }

cleanup() {
	for g in "${groups[@]}"; do
		for n in $(seq 0 $((NODES - 1))); do
			[ -n "${RESULTS_DIR:-}" ] &&
				cp "$(node_dir "$g" "$n").log" "$RESULTS_DIR/isolation2-$g-node$n.log" 2> /dev/null
			"$BINDIR/pg_ctl" -D "$(node_dir "$g" "$n")" -m immediate stop > /dev/null 2>&1
		done
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

echo "isolation2: part of Cloudberry's isolation2_schedule, on a coordinator and three segments"
printf '  of the %d tests of the schedule the manifest lists: %d run here, in %d groups, %d are skipped\n' \
	"$(awk '$1 == "run" || $1 == "skip"' "$HERE/manifest" | wc -l)" \
	"${#run_tests[@]}" "${#groups[@]}" \
	"$(awk '$1 == "skip"' "$HERE/manifest" | wc -l)"
awk '$1 == "skip" { $1 = ""; sub(/^ /, ""); print "  skip " $0 }' "$HERE/manifest" | cut -c1-150
echo

# A group's cluster, as the greenplum suite makes one.  Every node's log is
# the file beside its data directory, which pg_ctl() in setup.sql relies on.
make_cluster() {
	local g="$1" gi="$2" conf="$WORK/$1/gp_cluster.conf" n

	mkdir -p "$WORK/$g"
	{
		echo "# dbid content role host port datadir"
		for n in $(seq 0 $((NODES - 1))); do
			echo "$((n + 1)) $((n - 1)) p $(node_sock "$g" "$n") $(node_port "$gi" "$n") $(node_dir "$g" "$n")"
		done
	} > "$conf"
	for n in $(seq 0 $((NODES - 1))); do
		mkdir -p "$(node_sock "$g" "$n")"
		"$BINDIR/initdb" -D "$(node_dir "$g" "$n")" -N --locale=C --encoding=UTF8 \
			> "$WORK/$g/initdb$n.log" 2>&1 \
			|| { echo "initdb failed for node $n of group $g"; tail -20 "$WORK/$g/initdb$n.log"; return 1; }
		{
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "unix_socket_directories = '$(node_sock "$g" "$n")'"
			echo "listen_addresses = ''"
			echo "port = $(node_port "$gi" "$n")"
			echo "fsync = off"
			echo "gp.cluster_config = '$conf'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			echo "max_prepared_transactions = 64"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		} >> "$(node_dir "$g" "$n")/postgresql.auto.conf"
	done
	for n in $(seq 1 $((NODES - 1))) 0; do
		"$BINDIR/pg_ctl" -D "$(node_dir "$g" "$n")" -l "$(node_dir "$g" "$n").log" -w -t 60 start \
			> /dev/null 2>&1 \
			|| { echo "node $n of group $g did not start"; tail -20 "$(node_dir "$g" "$n").log"; return 1; }
	done
	# The driver asks database postgres which nodes there are, when a test
	# runs a shell command with one's address, and some tests run there: it
	# has gp_core too, and so its gp_segment_configuration.
	PGHOST="$(node_sock "$g" 0)" PGPORT="$(node_port "$gi" 0)" \
		"$PSQL" -X -q -d postgres -c "CREATE EXTENSION gp_core" > /dev/null 2>&1
	return 0
}
for gi in "${!groups[@]}"; do
	make_cluster "${groups[$gi]}" "$gi" &
done
for g in "${groups[@]}"; do
	wait -n || exit 1
done

# The settings the port has, respelled as the greenplum suite respells them;
# and, in the expected output, every word that names one, which is where a
# SHOW's column header has it too -- "gp_" and "gp." are the same length.
PGHOST="$(node_sock "${groups[0]}" 0)" PGPORT="$(node_port 0 0)" \
"$PSQL" -X -q -t -A -d postgres -c "SELECT name FROM pg_settings WHERE name LIKE 'gp.%' ORDER BY length(name) DESC" |
while read -r name; do
	short="${name#gp.}"
	case "$short" in
		optimizer*|statement_mem|enable_parallel|enable_groupagg|test_print_*)
			cbname="$short" ;;
		*) cbname="gp_$short" ;;
	esac
	printf 's/\\b(set|reset|show)(\\s+(local|session|system)\\s+|\\s+)%s\\b/\\1\\2%s/gI\n' "$cbname" "$name"
	printf 's/\\b(alter\\s+system\\s+(set|reset)\\s+)%s\\b/\\1%s/gI\n' "$cbname" "$name"
	printf "s/\\\\b(current_setting|set_config)\\\\('%s'/\\\\1('%s'/gI\n" "$cbname" "$name"
	case "$cbname" in
		gp_*) printf 's/^( *)%s( *)$/\\1%s\\2/\n' "$cbname" "$name" ;;
	esac
done > "$WORK/respell.sed"

# The driver, less "-c gp_role=utility"; run from Cloudberry's directory, as
# it sources global_sh_executor.sh from there.
sed 's/given_opt="-c gp_role=utility"/given_opt=None/' "$CB/sql_isolation_testcase.py" \
	> "$EXEC/sql_isolation_testcase.py"

mkdir -p "$WORK/gpdiff"
cp "$GPDIFF"/gpdiff.pl "$GPDIFF"/atmsort.pm "$GPDIFF"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$GPDIFF/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

gpdiff() {
	env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" \
		-I HINT: -I CONTEXT: -I GP_IGNORE: "$@"
}

convert() {
	sed -e "s#@abs_srcdir@#$CB#g" \
	    -e "s#@abs_builddir@#$CB#g" \
	    -e "s#@bindir@#$BINDIR#g" \
	    -e "s#@libdir@#${PG_REGRESS_SUITE:-/cb/pgregress}#g" \
	    -e "s#@DLSUFFIX@#.so#g" "$1"
}

# One group's tests, in one pass, on its cluster: a line "ok" or "bad" and
# the test in R/status for each, and what differs in R/regression.diffs.
run_group() {
	local g="$1" gi="$2" pass="$3" optimizer="$4"
	local R="$WORK/$g/$pass" t res exp name dir out i
	export PGHOST="$(node_sock "$g" 0)" PGPORT="$(node_port "$gi" 0)"

	mkdir -p "$R/results" "$R/canon" "$R/sql" "$R/expected"
	: > "$R/status"
	"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS isolation2test" > /dev/null 2>&1
	"$PSQL" -X -q -d postgres -c "CREATE DATABASE isolation2test" > /dev/null
	if ! out=$(sed -e "s#@BINDIR@#$BINDIR#g" "$HERE/setup.sql" |
			   "$PSQL" -X -q -v ON_ERROR_STOP=1 -d isolation2test -f - 2>&1); then
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

		if [ -f "$CB/input/$t.source" ]; then
			convert "$CB/input/$t.source" | sed -E -f "$WORK/respell.sed" > "$R/sql/$t.sql"
		else
			sed -E -f "$WORK/respell.sed" "$CB/sql/$t.sql" > "$R/sql/$t.sql"
		fi
		exp="$CB/expected/$t.out"
		[ "$pass" = orca ] && [ -f "$CB/expected/${t}_optimizer.out" ] && exp="$CB/expected/${t}_optimizer.out"
		[ -f "$CB/output/$t.source" ] && exp="$CB/output/$t.source"
		name="$(basename "$exp" .out)"
		name="${name%.source}"
		convert "$exp" | sed -E -f "$WORK/respell.sed" > "$R/expected/$t.out"

		# As pg_isolation2_regress runs it, from the suite's directory.
		( cd "$CB" && PGOPTIONS="-c gp.optimizer=$optimizer" \
			timeout 600 python3 "$EXEC/sql_isolation_testcase.py" \
				--dbname=isolation2test --initfile_prefix="$res" \
				< "$R/sql/$t.sql" > "$res" 2>&1 )

		# The port's first: what it takes off a segment's error -- which
		# segment -- leaves the lines Cloudberry's init files mask.
		inits=(--gpd_init "$HERE/init_file" --gpd_init "$GPDIFF/init_file"
		       --gpd_init "$CB/init_file_isolation2")
		[ -s "$res.initfile" ] && inits+=(--gpd_init "$res.initfile")

		gpdiff -U0 "${inits[@]}" "$R/expected/$t.out" "$res" 2> /dev/null |
			perl "$HERE/../singlenode/canon.pl" > "$R/canon/$t.diff"
		# A comparison that could not be made is a difference, never an empty one.
		st=("${PIPESTATUS[@]}")
		[ "${st[0]}" -le 1 ] && [ "${st[1]}" -eq 0 ] || echo "no comparison was made" >> "$R/canon/$t.diff"
		dir="$HERE/cloudberry/$(dirname "$t")"
		if [ ! -s "$R/canon/$t.diff" ] ||
		   cmp -s "$R/canon/$t.diff" "$dir/$name.$pass.diff" ||
		   cmp -s "$R/canon/$t.diff" "$dir/$name.diff"; then
			rm -f "$R/canon/$t.diff"
			echo "ok $t" >> "$R/status"
		else
			echo "bad $t" >> "$R/status"
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
	# says so.
	total=0; bad=0
	for t in "${run_tests[@]}"; do
		total=$((total + 1))
		results=$(for g in "${groups[@]}"; do
					  awk -v t="$t" '$2 == t { print $1 }' "$WORK/$g/$pass/status" 2> /dev/null
				  done)
		if [ -n "$results" ] && ! grep -qv '^ok$' <<< "$results"; then
			printf '  ok     %s\n' "$t"
		else
			bad=$((bad + 1))
			printf '  NOT OK %s\n' "$t"
		fi
	done
	echo "  Cloudberry's tests: $((total - bad)) of $total passed"
	if [ "$bad" -ne 0 ] || [ "$setup_failed" -ne 0 ]; then
		failed=$((failed + 1))
		if [ -n "${RESULTS_DIR:-}" ]; then
			for g in "${groups[@]}"; do
				mkdir -p "$RESULTS_DIR/isolation2-$pass/$g"
				cp -r "$WORK/$g/$pass/results" "$WORK/$g/$pass/canon" \
					"$RESULTS_DIR/isolation2-$pass/$g/" 2> /dev/null
				cat "$WORK/$g/$pass/regression.diffs" >> "$RESULTS_DIR/isolation2-$pass.diffs" 2> /dev/null
			done
		fi
	fi
	echo
done

[ "$failed" -eq 0 ]
