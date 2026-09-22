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
# Cloudberry's files are changed as the singlenode suite changes them:
# input/ and output/ .source files converted as Cloudberry's pg_regress
# converts them, and a setting the port has spelled as the port spells it.
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

if [ ! -f "$CB/greenplum_schedule" ] || [ ! -f "$PGSUITE/sql/test_setup.sql" ] ||
   [ ! -f "$PGSUITE/regress.so" ] || [ ! -x "$PG_REGRESS" ] ||
   [ ! -f "$CB/gpdiff.pl" ]; then
	echo "a test suite, pg_regress or gpdiff.pl is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-greenplum-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbg-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec
# (see the singlenode suite).
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-greenplum-XXXXXX")"
BASEPORT="${PGPORT:-$((7300 + RANDOM % 200))}"
NODES=4					# a coordinator and Cloudberry's three segments
PRELOAD='gp_core,gp_orca,gp_sql'
SECRET="greenplum-schedule-$RANDOM$RANDOM$RANDOM"

port() { echo $((BASEPORT + $1)); }
export PGPORT="$(port 0)" PGHOST="$SOCK/n0"

WATCHDOG=
cleanup() {
	[ -n "$WATCHDOG" ] && kill "$WATCHDOG" 2> /dev/null
	for n in $(seq 0 $((NODES - 1))); do
		[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/node$n.log" "$RESULTS_DIR/greenplum-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$WORK/node$n" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

run_tests=$(awk '$1 == "run" { print $2 }' "$HERE/manifest")

echo "greenplum: part of Cloudberry's greenplum_schedule, on a coordinator and three segments"
printf '  of the %d tests of the schedule the manifest lists: %d run here, %d are skipped\n' \
	"$(awk '$1 == "run" || $1 == "skip"' "$HERE/manifest" | wc -l)" \
	"$(echo "$run_tests" | wc -w)" \
	"$(awk '$1 == "skip"' "$HERE/manifest" | wc -l)"
echo

# The cluster, as run.sh of the cluster suite makes one.
CONF="$WORK/gp_cluster.conf"
{
	echo "# dbid content role host port datadir"
	for n in $(seq 0 $((NODES - 1))); do
		echo "$((n + 1)) $((n - 1)) p $SOCK/n$n $(port "$n") $WORK/node$n"
	done
} > "$CONF"
for n in $(seq 0 $((NODES - 1))); do
	mkdir -p "$SOCK/n$n"
	"$BINDIR/initdb" -D "$WORK/node$n" -N --locale=C --encoding=UTF8 > "$WORK/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$WORK/initdb$n.log"; exit 1; }
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$SOCK/n$n'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "fsync = off"
		echo "gp.cluster_config = '$CONF'"
		echo "gp.dbid = $((n + 1))"
		echo "gp.cluster_secret = '$SECRET'"
		echo "max_prepared_transactions = 2"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$WORK/node$n/postgresql.auto.conf"
done
for n in $(seq 1 $((NODES - 1))) 0; do
	"$BINDIR/pg_ctl" -D "$WORK/node$n" -l "$WORK/node$n.log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "node $n did not start"; tail -20 "$WORK/node$n.log"; exit 1; }
done

# The settings the port has, as sed that respells Cloudberry's names for
# them; the singlenode suite says how.
"$PSQL" -X -q -t -A -d postgres -c "SELECT name FROM pg_settings WHERE name LIKE 'gp.%' ORDER BY length(name) DESC" |
while read -r name; do
	short="${name#gp.}"
	case "$short" in
		optimizer*) cbname="$short" ;;
		*) cbname="gp_$short" ;;
	esac
	printf 's/\\b(set|reset|show)(\\s+(local|session)\\s+|\\s+)%s\\b/\\1\\2%s/gI\n' "$cbname" "$name"
	printf "s/\\\\b(current_setting|set_config)\\\\('%s'/\\\\1('%s'/gI\n" "$cbname" "$name"
done > "$WORK/respell.sed"

convert() {
	sed -e "s#@abs_srcdir@#$CB#g" \
	    -e "s#@abs_builddir@#$CB#g" \
	    -e "s#@testtablespace@#$WORK/testtablespace#g" \
	    -e "s#@libdir@#$PGSUITE#g" \
	    -e "s#@DLSUFFIX@#.so#g" "$1"
}

# The suite: PostgreSQL 19's test_setup, the port's setup, then Cloudberry's.
SN="$WORK/suite"
mkdir -p "$SN/sql" "$SN/expected"
cp "$PGSUITE/sql/test_setup.sql" "$SN/sql/"
cp "$PGSUITE/expected/test_setup.out" "$SN/expected/"
# test_setup reads its data from the directory it runs from, --inputdir
cp -r "$PGSUITE/data" "$SN/data"
cp "$HERE"/sql/*.sql "$SN/sql/"
cp "$HERE"/expected/*.out "$SN/expected/" 2> /dev/null
{ echo "test: test_setup"; echo "test: gp_setup"; } > "$SN/schedule"
echo "gp_setup" > "$SN/port_tests"
: > "$SN/cloudberry_tests"
for t in $run_tests; do
	mkdir -p "$SN/sql/$(dirname "$t")" "$SN/expected/$(dirname "$t")"
	if [ -f "$CB/input/$t.source" ]; then
		convert "$CB/input/$t.source" | sed -E -f "$WORK/respell.sed" > "$SN/sql/$t.sql"
	else
		sed -E -f "$WORK/respell.sed" "$CB/sql/$t.sql" > "$SN/sql/$t.sql"
	fi
	if [ -f "$CB/output/$t.source" ]; then
		convert "$CB/output/$t.source" | sed -E -f "$WORK/respell.sed" > "$SN/expected/$t.out"
	else
		for f in "$CB/expected/$t.out" "$CB"/expected/"$t"_[0-9].out; do
			[ -f "$f" ] && sed -E -f "$WORK/respell.sed" "$f" > "$SN/expected/$(dirname "$t")/$(basename "$f")"
		done
	fi
	echo "test: $t" >> "$SN/schedule"
	echo "$t" >> "$SN/cloudberry_tests"
done
mkdir -p "$WORK/testtablespace"

mkdir -p "$WORK/gpdiff"
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, for PostgreSQL's test_setup too, whose
# tables are distributed here and say so; with a difference reviewed and
# kept counted as none (canon.pl, cloudberry/, as the singlenode suite does).
mkdir -p "$EXEC/bin"
cat > "$EXEC/bin/diff" <<EOF
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
    --gpd_init "$CB/init_file" --gpd_init "$HERE/init_file")
canon="$WORK/\$CB_DIFF_MODE/canon/\$name.diff"
env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U0 "\${cb[@]}" "\$exp" "\$res" 2> /dev/null |
	perl "$HERE/../singlenode/canon.pl" > "\$canon"
if [ ! -s "\$canon" ] || cmp -s "\$canon" "$HERE/cloudberry/\$name.\$CB_DIFF_MODE.diff" ||
   cmp -s "\$canon" "$HERE/cloudberry/\$name.diff"; then
	rm -f "\$canon"
	exit 0
fi
exec env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" "\${opts[@]}" "\${cb[@]}" "\$exp" "\$res"
EOF
chmod +x "$EXEC/bin/diff"

# A statement that runs for minutes is a finding, as the singlenode suite
# says; so is one that waits for ever on a lock a segment holds.
TIMEOUT="${STATEMENT_TIMEOUT:-60 seconds}"
watchdog() {
	local pid query
	while :; do
		sleep 5
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

failed=0
for pass in ${PASSES:-planner orca}; do
	echo "== pass: $pass"
	mkdir -p "$WORK/$pass/canon"
	case "$pass" in
		planner) optimizer=off ;;
		orca)    optimizer=on ;;
	esac
	watchdog "$WORK/$pass/cancelled" &
	WATCHDOG=$!
	# From Cloudberry's suite's directory, as its Makefile runs it: its tests
	# read data/ by relative paths.
	cd "$CB"
	PATH="$EXEC/bin:$PATH" CB_DIFF_MODE="$pass" PGOPTIONS="-c gp.optimizer=$optimizer" \
		"$PG_REGRESS" \
			--bindir="$BINDIR" \
			--inputdir="$SN" \
			--expecteddir="$SN" \
			--outputdir="$WORK/$pass" \
			--dlpath="$PGSUITE" \
			--schedule="$SN/schedule" \
			--max-connections=20 \
			--host="$SOCK/n0" --port="$(port 0)" \
		> "$WORK/$pass/pg_regress.out" 2>&1
	rc=$?
	kill "$WATCHDOG" 2> /dev/null; wait "$WATCHDOG" 2> /dev/null; WATCHDOG=

	# test_setup's tablespace is the coordinator's alone, as every
	# tablespace is, and outlives the database: gone before the next pass.
	"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS regression" \
		-c "DROP TABLESPACE IF EXISTS regress_tblspace" > /dev/null 2>&1

	grep -E "^(not )?ok " "$WORK/$pass/pg_regress.out" | sed 's/^/  /'
	total=$(grep -cE "^(not )?ok " "$WORK/$pass/pg_regress.out")
	bad=$(grep -cE "^not ok " "$WORK/$pass/pg_regress.out")
	echo "  $((total - bad)) of $total passed"
	if [ -s "$WORK/$pass/cancelled" ]; then
		echo "  cancelled after $TIMEOUT:"
		sed 's/^/    /' "$WORK/$pass/cancelled"
	fi
	sed -nE 's/^ok +[0-9]+ +[-+] +([^ ]+) .*/\1/p' "$WORK/$pass/pg_regress.out" |
	while read -r t; do
		rm -f "$WORK/$pass/canon/$t.diff" "$WORK/$pass/canon/$t"_[0-9].diff
	done
	unreviewed=$(cd "$WORK/$pass/canon" && ls -- *.diff 2> /dev/null | sed 's/\.diff$//')
	if [ -n "$unreviewed" ]; then
		echo "  differences no one has reviewed, from the expected output named:"
		echo $unreviewed | fold -s -w 72 | sed 's/^/    /'
	fi
	if [ "$rc" -ne 0 ]; then
		failed=$((failed + 1))
		if [ -n "${RESULTS_DIR:-}" ]; then
			cp "$WORK/$pass/regression.diffs" "$RESULTS_DIR/greenplum-$pass.diffs" 2> /dev/null
			mkdir -p "$RESULTS_DIR/greenplum-$pass-canon" "$RESULTS_DIR/greenplum-$pass-results"
			cp "$WORK/$pass"/canon/*.diff "$RESULTS_DIR/greenplum-$pass-canon/" 2> /dev/null
			cp -r "$WORK/$pass"/results/. "$RESULTS_DIR/greenplum-$pass-results/" 2> /dev/null
		fi
	fi
	echo
done

[ "$failed" -eq 0 ]
