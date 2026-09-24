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
#
# Each pass is spread over SHARDS servers (8 unless said), one run_test.pl
# each, side by side, and every server makes its database, installs the
# extensions and checks their uninstall itself, as the whole suite does on
# one.  Which tests each runs is below.

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

SHARDS="${SHARDS:-8}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-postgis-regress-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbp-XXXXXX)"
PORT="${PGPORT:-$((7300 + RANDOM % 200))}"

cleanup() {
	for k in $(seq 1 "$SHARDS"); do
		[ -n "${RESULTS_DIR:-}" ] &&
			cp "$WORK/s$k/log" "$RESULTS_DIR/postgis-regress-server-$k.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$WORK/s$k/data" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

{ read -r FLAGS; read -r HOOKS; read -r TESTS; } < "$TREE/regress/port-regress.txt"

# The tests each server runs.  Dealt out by how long each takes (durations),
# the longest first, each to the server with the least so far; then each
# server's in PostGIS's order.  Three are every server's, where it needs
# them: addtosearchpath undoes what the topology hook made before the
# extension was installed, and must, for the extension to uninstall, so it
# comes before the server's first topology test, or last; load_outdb and
# raster's clean come before and after its raster tests.
ADDTOSEARCHPATH=./topology/test/regress/addtosearchpath.sql
FIRST=./raster/test/regress/loader/load_outdb
LAST=./raster/test/regress/clean
declare -A weight=()
while read -r ms t; do
	weight[$t]="$ms"
done < <(grep -v '^#' "$HERE/durations")
order=()
i=0
for t in $TESTS; do
	case "$t" in "$ADDTOSEARCHPATH"|"$FIRST"|"$LAST") i=$((i + 1)); continue ;; esac
	key="${t#./}"; key="${key%.sql}"
	order+=("${weight[$key]:-50} $i $t")
	i=$((i + 1))
done
load=(); picked=()
for k in $(seq 1 "$SHARDS"); do load[k]=0; done
while read -r ms i t; do
	best=1
	for k in $(seq 2 "$SHARDS"); do
		[ "${load[k]}" -lt "${load[best]}" ] && best=$k
	done
	load[best]=$((load[best] + ms))
	picked+=("$best $i $t")
done < <(printf '%s\n' "${order[@]}" | sort -k1,1nr -k2,2n)
for k in $(seq 1 "$SHARDS"); do
	list=""; raster=0; topology=0
	while read -r _ _ t; do
		if [[ "$t" == ./topology/test/regress/* ]] && [ "$topology" -eq 0 ]; then
			list="$list $ADDTOSEARCHPATH"; topology=1
		fi
		if [[ "$t" == ./raster/test/regress/* ]] && [ "$raster" -eq 0 ]; then
			list="$list $FIRST"; raster=1
		fi
		list="$list $t"
	done < <(printf '%s\n' "${picked[@]}" | awk -v k="$k" '$1 == k' | sort -k2,2n)
	[ "$topology" -eq 0 ] && list="$list $ADDTOSEARCHPATH"
	[ "$raster" -eq 1 ] && list="$list $LAST"
	shard_tests[k]="$list"
done

# What a server's report counts that another's counts too: its uninstall
# check, two tests of run_test.pl's, addtosearchpath, and load_outdb and
# clean.  The totals below count each once, as the suite on one server does.
raster_servers=$(for k in $(seq 1 "$SHARDS"); do echo "${shard_tests[k]}"; done | grep -c -- "$FIRST")
repeated=$(( 3 * (SHARDS - 1) ))
[ "$raster_servers" -gt 1 ] && repeated=$(( repeated + 2 * (raster_servers - 1) ))

echo "postgis_regress: PostGIS's own regression suite, with every M1 module loaded"
echo "  PostGIS $(cat "$TREE/.postgis_commit" 2>/dev/null || echo '?'), $(echo "$TESTS" | wc -w) tests, run_test.pl $FLAGS, over $SHARDS servers"
echo

# The servers, made and started side by side.
start_server() {
	local k="$1" dir="$WORK/s$1"

	mkdir -p "$dir" "$SOCK/$k"
	"$BINDIR/initdb" -D "$dir/data" -N --locale=C --encoding=UTF8 > "$dir/initdb.log" 2>&1 \
		|| { echo "initdb failed for server $k"; tail -20 "$dir/initdb.log"; return 1; }
	{
		echo "unix_socket_directories = '$SOCK/$k'"
		echo "listen_addresses = ''"
		echo "port = $PORT"
		echo "fsync = off"
		echo "shared_preload_libraries = 'gp_core,gp_orca,gp_task,gp_matview,gp_sql,gp_security'"
	} >> "$dir/data/postgresql.conf"
	"$BINDIR/pg_ctl" -D "$dir/data" -l "$dir/log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "server $k did not start"; tail -20 "$dir/log"; return 1; }
}
for k in $(seq 1 "$SHARDS"); do
	start_server "$k" &
done
for k in $(seq 1 "$SHARDS"); do
	wait -n || exit 1
done

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
	# there.  Each server's own run, side by side, then their reports as one.
	for k in $(seq 1 "$SHARDS"); do
		# shellcheck disable=SC2086 -- the flags, hooks and tests are lists
		( cd "$TREE" &&
		  PGHOST="$SOCK/$k" PGPORT="$PORT" PGOPTIONS="$options" \
		  POSTGIS_TOP_BUILD_DIR="$TREE" PGIS_REG_TMPDIR="$WORK/$pass/tmp/$k" \
		  POSTGIS_REGRESS_DB="postgis_reg" \
			perl regress/run_test.pl $FLAGS $HOOKS ${shard_tests[k]} ) \
			> "$WORK/$pass/run_test.$k.out" 2>&1 &
	done
	wait
	cat "$WORK/$pass"/run_test.*.out > "$WORK/$pass/run_test.out"

	run=0; bad=0
	for k in $(seq 1 "$SHARDS"); do
		r=$(sed -n 's/^Run tests: //p' "$WORK/$pass/run_test.$k.out")
		b=$(sed -n 's/^Failed: //p' "$WORK/$pass/run_test.$k.out")
		# A server whose run did not get as far as its report fails the pass.
		if [ -z "$r" ] || [ -z "$b" ]; then
			echo "  server $k made no report:"
			tail -5 "$WORK/$pass/run_test.$k.out" | sed 's/^/    /'
			run=""; break
		fi
		run=$((run + r)); bad=$((bad + b))
	done
	[ -n "$run" ] && run=$((run - repeated))

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
