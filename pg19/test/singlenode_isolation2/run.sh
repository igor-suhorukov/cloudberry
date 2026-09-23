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
# Cloudberry's singlenode isolation2 suite, as the port runs it, against every
# M1 module.
#
# src/test/singlenode_isolation2 is the tests Cloudberry runs in its
# single-node mode that need more than one session at a time, written in its
# isolation2 syntax (1: ..., 2&: ..., 2<:) and run by its own driver,
# sql_isolation_testcase.py, over PyGreSQL.  The port runs the driver as
# Cloudberry's pg_isolation2_regress does -- the test on its standard input,
# the result on its standard output -- and compares the result with gpdiff.pl
# under Cloudberry's init files, as Cloudberry's pg_regress does.  The one
# change is the setup: setup.sql here, in place of Cloudberry's, says why.
#
# manifest says of each test the schedule names whether it runs, and why not.
# A test passes if its result is Cloudberry's expected output, or differs from
# it by exactly a difference reviewed and kept in cloudberry/ (see
# cloudberry/README, and ../singlenode/canon.pl for the form).
#
# Two passes, as the singlenode suite has: gp.optimizer = off, Cloudberry's
# single-node configuration, and on, the port's default.  Cloudberry's own
# expected output for a test under ORCA, test_optimizer.out, is the one used
# in the second pass where there is one.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CB="${CB_ISOLATION2_DIR:-/cb/src/test/singlenode_isolation2}"
GPDIFF="${GPDIFF_DIR:-/cb/src/test/regress}"

if [ ! -f "$CB/sql_isolation_testcase.py" ] || [ ! -f "$GPDIFF/gpdiff.pl" ] ||
   [ ! -f "$GPDIFF/init_file" ] || ! python3 -c 'import pg' 2> /dev/null; then
	echo "Cloudberry's isolation2 suite, gpdiff.pl or PyGreSQL is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-isolation2-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbi-XXXXXX)"
PORT="${PGPORT:-$((7700 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK"

cleanup() {
	[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/log" "$RESULTS_DIR/isolation2-server.log" 2> /dev/null
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

run_tests=$(awk '$1 == "run" { print $2 }' "$HERE/manifest")

echo "singlenode_isolation2: Cloudberry's singlenode isolation2 suite, with every M1 module loaded"
printf '  of the %d tests Cloudberry schedules: %d run here, %d are skipped\n' \
	"$(grep -cE '^(run|skip) ' "$HERE/manifest")" \
	"$(echo "$run_tests" | wc -w)" \
	"$(grep -c '^skip ' "$HERE/manifest")"
awk '$1 == "skip" { $1 = ""; sub(/^ /, ""); print "  skip " $0 }' "$HERE/manifest" | cut -c1-150
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "fsync = off"
	echo "shared_preload_libraries = 'gp_core,gp_orca,gp_task,gp_matview,gp_sql,gp_security'"
} >> "$WORK/data/postgresql.conf"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

mkdir -p "$WORK/gpdiff"
cp "$GPDIFF"/gpdiff.pl "$GPDIFF"/atmsort.pm "$GPDIFF"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$GPDIFF/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# Cloudberry's pg_regress compares with these; gpdiff.pl runs diff from PATH.
gpdiff() {
	env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" \
		-I HINT: -I CONTEXT: -I GP_IGNORE: "$@"
}

failed=0
for pass in ${PASSES:-planner orca}; do
	echo "== pass: $pass"
	R="$WORK/$pass"
	mkdir -p "$R/results" "$R/canon"
	case "$pass" in
		planner) optimizer=off ;;
		orca)    optimizer=on ;;
	esac

	"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS isolation2test" > /dev/null 2>&1
	"$PSQL" -X -q -d postgres -c "CREATE DATABASE isolation2test" > /dev/null
	if ! out=$("$PSQL" -X -q -v ON_ERROR_STOP=1 -d isolation2test -f "$HERE/setup.sql" 2>&1); then
		echo "  the setup failed:"; printf '%s\n' "$out" | sed 's/^/    /'
		failed=$((failed + 1)); continue
	fi

	total=0; bad=0
	for t in $run_tests; do
		total=$((total + 1))
		res="$R/results/$t.out"
		mkdir -p "$(dirname "$res")" "$(dirname "$R/canon/$t")"

		# As pg_isolation2_regress runs it, from the suite's directory.
		( cd "$CB" && PGOPTIONS="-c gp.optimizer=$optimizer" \
			timeout 300 python3 ./sql_isolation_testcase.py \
				--dbname=isolation2test --initfile_prefix="$res" \
				< "sql/$t.sql" > "$res" 2>&1 )

		exp="$CB/expected/$t.out"
		[ "$pass" = orca ] && [ -f "$CB/expected/${t}_optimizer.out" ] && exp="$CB/expected/${t}_optimizer.out"
		inits=(--gpd_init "$GPDIFF/init_file" --gpd_init "$CB/init_file_isolation2"
		       --gpd_init "$HERE/init_file")
		[ -s "$res.ini" ] && inits+=(--gpd_init "$res.ini")

		gpdiff -U0 "${inits[@]}" "$exp" "$res" 2> /dev/null | perl "$HERE/../singlenode/canon.pl" > "$R/canon/$t.diff"
		# A comparison that could not be made is a difference, never an empty one.
		st=("${PIPESTATUS[@]}")
		[ "${st[0]}" -le 1 ] && [ "${st[1]}" -eq 0 ] || echo "no comparison was made" >> "$R/canon/$t.diff"
		name="$(basename "$exp" .out)"
		dir="$HERE/cloudberry/$(dirname "$t")"
		if [ ! -s "$R/canon/$t.diff" ] ||
		   cmp -s "$R/canon/$t.diff" "$dir/$name.$pass.diff" ||
		   cmp -s "$R/canon/$t.diff" "$dir/$name.diff"; then
			rm -f "$R/canon/$t.diff"
			printf '  ok     %s\n' "$t"
		else
			bad=$((bad + 1))
			printf '  NOT OK %s\n' "$t"
			gpdiff -U3 "${inits[@]}" "$exp" "$res" >> "$R/regression.diffs" 2> /dev/null
		fi
	done
	echo "  Cloudberry's tests: $((total - bad)) of $total passed"
	if [ "$bad" -ne 0 ]; then
		failed=$((failed + 1))
		if [ -n "${RESULTS_DIR:-}" ]; then
			mkdir -p "$RESULTS_DIR/isolation2-$pass"
			cp -r "$R/results" "$R/canon" "$RESULTS_DIR/isolation2-$pass/" 2> /dev/null
			cp "$R/regression.diffs" "$RESULTS_DIR/isolation2-$pass.diffs" 2> /dev/null
		fi
	fi
	echo
done

[ "$failed" -eq 0 ]
