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
# PostGIS's own regression suite, against the port with every M1 module
# preloaded: "PostGIS on one node", which M1's test column asks for.
#
# PostGIS 3.7.0rc2's tests, from the image's build of it: the core suite,
# the loader and dumper, raster and topology -- what its "make installcheck"
# runs against the extensions as installed, with the flags and hook scripts
# its configure chose (written into the image as regress/port-regress.txt;
# see Dockerfile.cbext).  Not its upgrade pass, which needs older PostGIS
# releases installed beside this one.  PostGIS's run_test.pl drives it and
# compares, as PostGIS's own CI does.
#
# Two passes, each over a fresh database:
#
#   planner   gp.optimizer = off.  PostGIS on PostgreSQL 19 with the port's
#             modules loaded, which is the baseline: what fails here fails
#             for a reason that is not ORCA's.
#   orca      gp.optimizer = on, the port's default: every query PostGIS's
#             tests make is ORCA's to plan, spatial predicates through the
#             rewrite in front of it.  ORCA's report of the columns it has no
#             statistics for is off (gp.optimizer_print_missing_stats):
#             PostGIS's tests analyze few of their tables, and run_test.pl
#             compares with diff, so it would be in every answer.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
export PATH="$BINDIR:$PATH"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="${POSTGIS_BUILD_TREE:-/postgis}"

if [ ! -f "$TREE/regress/run_test.pl" ] || [ ! -f "$TREE/regress/port-regress.txt" ] ||
   [ ! -f "$("$BINDIR/pg_config" --pkglibdir)/gp_orca.so" ]; then
	echo "PostGIS's build tree or gp_orca is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-postgis-regress-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbp-XXXXXX)"
PORT="${PGPORT:-$((7300 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK"

cleanup() {
	[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/log" "$RESULTS_DIR/postgis-regress-server.log" 2> /dev/null
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

{ read -r FLAGS; read -r HOOKS; read -r TESTS; } < "$TREE/regress/port-regress.txt"

echo "postgis_regress: PostGIS's own regression suite, with every M1 module loaded"
echo "  PostGIS $(cat "$TREE/.postgis_commit" 2>/dev/null || echo '?'), $(echo "$TESTS" | wc -w) tests, run_test.pl $FLAGS"
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

failed=0
for pass in ${PASSES:-planner orca}; do
	echo "== pass: $pass"
	mkdir -p "$WORK/$pass"
	case "$pass" in
		planner) options="-c gp.optimizer=off" ;;
		orca)    options="-c gp.optimizer=on -c gp.optimizer_print_missing_stats=off" ;;
	esac
	# From the top of the tree, as PostGIS's installcheck runs it
	# (regress/runtest.mk): the tests and the hook scripts are named from
	# there.
	# shellcheck disable=SC2086 -- the flags, hooks and tests are lists
	( cd "$TREE" &&
	  PGOPTIONS="$options" POSTGIS_TOP_BUILD_DIR="$TREE" PGIS_REG_TMPDIR="$WORK/$pass/tmp" \
	  POSTGIS_REGRESS_DB="postgis_reg" \
		perl regress/run_test.pl $FLAGS $HOOKS $TESTS ) > "$WORK/$pass/run_test.out" 2>&1
	rc=$?

	run=$(sed -n 's/^Run tests: //p' "$WORK/$pass/run_test.out")
	bad=$(sed -n 's/^Failed: //p' "$WORK/$pass/run_test.out")

	# A test whose output differs from PostGIS's expected output by exactly a
	# difference reviewed and kept in <pass>/ -- in the form
	# ../singlenode/canon.pl gives it, and <pass>/README saying why -- is
	# counted as passing.  Any other failure is left in canon/ for review.
	mkdir -p "$WORK/$pass/canon"
	reviewed=0; unreviewed=0
	while read -r name difffile; do
		exp=$(sed -n '1s/^--- \([^\t ]*\).*/\1/p' "$difffile")
		obt=$(sed -n '2s/^+++ \([^\t ]*\).*/\1/p' "$difffile")
		canon="$WORK/$pass/canon/$name.diff"
		mkdir -p "$(dirname "$canon")"
		( cd "$TREE" && diff -U0 "$exp" "$obt" ) | perl "$HERE/../singlenode/canon.pl" > "$canon"
		# A comparison that could not be made is a difference, never an empty one.
		st=("${PIPESTATUS[@]}")
		[ "${st[0]}" -le 1 ] && [ "${st[1]}" -eq 0 ] || echo "no comparison was made" >> "$canon"
		if cmp -s "$canon" "$HERE/$pass/$name.diff"; then
			reviewed=$((reviewed + 1)); rm -f "$canon"
		else
			unreviewed=$((unreviewed + 1))
			echo "  NOT OK $name"
		fi
	done < <(sed -nE 's#^ *(\./)?([^ ]+) \.\. failed \(diff expected obtained: ([^)]+)\)#\2 \3#p' "$WORK/$pass/run_test.out")
	# a failure that is not a difference in the output
	grep -E " failed \(" "$WORK/$pass/run_test.out" | grep -v "diff expected obtained" | sed 's/^/  /' | cut -c1-200
	other=$(grep -E " failed \(" "$WORK/$pass/run_test.out" | grep -vc "diff expected obtained")
	echo "  ${run:-?} run, ${bad:-?} failed: $reviewed by a difference reviewed and kept ($pass/), $((unreviewed + other)) not"
	if [ -z "$run" ] || [ "$((unreviewed + other))" -ne 0 ] || [ "$reviewed" != "${bad:-x}" ]; then
		failed=$((failed + 1))
		if [ -n "${RESULTS_DIR:-}" ]; then
			cp "$WORK/$pass/run_test.out" "$RESULTS_DIR/postgis-regress-$pass.out" 2> /dev/null
			mkdir -p "$RESULTS_DIR/postgis-regress-$pass"
			cp -r "$WORK/$pass/tmp/." "$RESULTS_DIR/postgis-regress-$pass/" 2> /dev/null
			mkdir -p "$RESULTS_DIR/postgis-regress-$pass-canon"
			cp -r "$WORK/$pass/canon/." "$RESULTS_DIR/postgis-regress-$pass-canon/" 2> /dev/null
		fi
	fi
	echo
done

[ "$failed" -eq 0 ]
