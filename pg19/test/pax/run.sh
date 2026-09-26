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
# Cloudberry's PAX tests, on a cluster: M5's.
#
# contrib/pax_storage/pax_schedule is what Cloudberry runs against its demo
# cluster -- a coordinator and three segments -- with PAX preloaded and made
# by initdb, in the database pax_test, with gp_inject_fault loaded
# (contrib/pax_storage/Makefile, pax-test), and PAX the default table access
# method (.github/workflows/build-cloudberry.yml).  The port runs it the same way,
# on a coordinator and three segments of its own, in the schedule's order and
# groups: manifest says of each test of the schedule whether it runs and, if
# not, why.  What Cloudberry has built in, the port has as extensions,
# gp_core's, gp_sql's, gp_ao's and pax's, in every database the tests use;
# PAX's catalogs, which Cloudberry's initdb put in pg_ext_aux, are in the
# schema pax, as the tests are read here -- each table's aux table is in
# pg_ext_aux, as Cloudberry's is; and a setting of Cloudberry's core is
# respelled as the port spells it, as the greenplum suite respells it.
#
# Each test is compared as Cloudberry's pg_regress compares it: gpdiff.pl
# under Cloudberry's init files, PAX's and the port's, or exactly a difference
# in cloudberry/, reviewed and kept (see ../singlenode/canon.pl for the form).
# One pass, under the planner: PAX's expected plans are the planner's, and a
# test that wants ORCA sets it.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAX="${CB_PAX_DIR:-/cb/contrib/pax_storage}"
CB="${CB_REGRESS_DIR:-/cb/src/test/regress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"

if [ ! -f "$PAX/pax_schedule" ] || [ ! -x "$PG_REGRESS" ] || [ ! -f "$CB/gpdiff.pl" ] ||
   [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/pax.control" ]; then
	echo "PAX's tests, pg_regress, gpdiff.pl or pax is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-pax-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbp-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-pax-XXXXXX")"
BASEPORT="${PGPORT:-$((7500 + RANDOM % 200))}"
NODES=4					# a coordinator and Cloudberry's three segments
PRELOAD='gp_core,gp_orca,gp_sql,gp_ao,pax'
SECRET="pax-schedule-$RANDOM$RANDOM$RANDOM"

port() { echo $((BASEPORT + $1)); }
# The superuser is Cloudberry's demo cluster's, gpadmin: its expected output
# names it.
export PGPORT="$(port 0)" PGHOST="$SOCK/n0" PGUSER=gpadmin

WATCHDOG=
cleanup() {
	[ -n "$WATCHDOG" ] && kill "$WATCHDOG" 2> /dev/null
	for n in $(seq 0 $((NODES - 1))); do
		[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/node$n.log" "$RESULTS_DIR/pax-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$WORK/node$n" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

# PAX_GROUP, a group of the manifest's, runs its tests and every group's
run_tests=$(awk -v g="${PAX_GROUP:-}" '
	$1 == "run" && (g == "" || $3 == "*" || ($3 == "" ? "main" : $3) == g) { print $2 }
	' "$HERE/manifest")
# PAX_TESTS, a list of the manifest's tests, runs those alone, setup first
if [ -n "${PAX_TESTS:-}" ]; then
	run_tests=$(for t in $run_tests; do
		[[ " setup $PAX_TESTS " == *" $t "* ]] && echo "$t"; done)
fi

echo "pax: Cloudberry's PAX tests, on a coordinator and three segments${PAX_GROUP:+, group $PAX_GROUP}"
printf '  of the %d tests of the schedule the manifest lists: %d run here, %d are skipped\n' \
	"$(awk '$1 == "run" || $1 == "skip"' "$HERE/manifest" | wc -l)" \
	"$(echo "$run_tests" | wc -w)" \
	"$(awk '$1 == "skip"' "$HERE/manifest" | wc -l)"
echo

# The cluster, as the greenplum suite makes one.
CONF="$WORK/gp_cluster.conf"
{
	echo "# dbid content role host port datadir"
	for n in $(seq 0 $((NODES - 1))); do
		echo "$((n + 1)) $((n - 1)) p $SOCK/n$n $(port "$n") $WORK/node$n"
	done
} > "$CONF"
for n in $(seq 0 $((NODES - 1))); do
	mkdir -p "$SOCK/n$n"
	"$BINDIR/initdb" -D "$WORK/node$n" -N -U gpadmin --locale=C --encoding=UTF8 > "$WORK/initdb$n.log" 2>&1 \
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
		echo "max_prepared_transactions = 64"
		# The coordinator logs every statement, as Cloudberry's demo
		# cluster's does: a test that asks for LOG messages is sent its own
		# statements' too, which its expected output has.
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'" && echo "log_statement = 'all'"
	} >> "$WORK/node$n/postgresql.auto.conf"
done
for n in $(seq 1 $((NODES - 1))) 0; do
	"$BINDIR/pg_ctl" -D "$WORK/node$n" -l "$WORK/node$n.log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "node $n did not start"; tail -20 "$WORK/node$n.log"; exit 1; }
done
# The port's extensions where the watchdog asks, and in template1, for the
# database of the tests and a database a test makes, as Cloudberry's initdb
# made PAX's objects in every database.
for db in postgres template1; do
	"$PSQL" -X -q -d "$db" -c "CREATE EXTENSION gp_core" -c "CREATE EXTENSION gp_sql" \
		-c "CREATE EXTENSION gp_ao" -c "CREATE EXTENSION pax" ||
		{ echo "could not create the port's extensions in $db"; exit 1; }
done

# The settings the port has, as sed that respells Cloudberry's names for
# them, as the greenplum suite makes it; and PAX's catalogs in
# pg_ext_aux, Cloudberry's schema of them, as the extension's.
PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -d postgres \
	-c "SELECT name FROM pg_settings WHERE name LIKE 'gp.%' ORDER BY length(name) DESC" |
while read -r name; do
	short="${name#gp.}"
	case "$short" in
		optimizer*|statement_mem|enable_parallel|enable_groupagg|test_print_*|\
		resource_scheduler|resource_select_only|resource_cleanup_gangs_on_wait|\
		max_resource_queues|max_resource_portals_per_transaction|max_statement_mem|\
		debug_resource_group|runaway_detector_activation_percent|\
		vmem_process_interrupt|explain_memory_verbosity|coredump_on_memerror)
			cbname="$short" ;;
		*) cbname="gp_$short" ;;
	esac
	printf 's/\\b([a-z_][a-z0-9_]*)\\.%s\\b/\\1."%s"/gI\n' "$cbname" "$name"
	printf 's/\\b(set|reset|show)(\\s+(local|session)\\s+|\\s+)%s\\b/\\1\\2%s/gI\n' "$cbname" "$name"
	printf "s/\\\\b(current_setting|set_config)\\\\('%s'/\\\\1('%s'/gI\n" "$cbname" "$name"
	case "$cbname" in gp_*)
		printf 's/^( +)%s( +)$/\\1%s\\2/\n' "$cbname" "$name" ;;
	esac
done > "$WORK/respell.sed"
echo 's/\bpg_ext_aux\.(pg_pax_tables|pg_pax_fastsequence|paxauxstats)\b/pax.\1/g' >> "$WORK/respell.sed"

# The suite, from PAX's directory, as its Makefile runs it.
SN="$WORK/pax"
mkdir -p "$SN/sql" "$SN/expected"
for t in $run_tests; do
	mkdir -p "$SN/sql/$(dirname "$t")" "$SN/expected/$(dirname "$t")"
	sed -E -f "$WORK/respell.sed" "$PAX/sql/$t.sql" > "$SN/sql/$t.sql"
	for e in "$PAX/expected/$t.out" "$PAX"/expected/"$t"_[0-9].out; do
		[ -f "$e" ] || continue
		sed -E -f "$WORK/respell.sed" "$e" > "$SN/expected/${e#"$PAX"/expected/}"
	done
done
# Cloudberry's schedule, its groups kept, less the tests the manifest skips
awk -v tests=" $(echo $run_tests) " '
	/^test:/ {
		line = ""
		for (i = 2; i <= NF; i++)
			if (index(tests, " " $i " ")) line = line " " $i
		if (line != "") print "test:" line
	}' "$PAX/pax_schedule" > "$SN/schedule"

mkdir -p "$WORK/gpdiff"
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, with a difference reviewed and kept
# counted as none, as the greenplum suite has it.  A test in a directory,
# statistics/statistics, is kept as statistics_statistics.diff.
mkdir -p "$EXEC/bin" "$WORK/canon"
cat > "$EXEC/bin/diff" <<EOF
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(echo "\${exp#$SN/expected/}" | sed 's/\.out\$//' | tr / _)
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
    --gpd_init "$CB/init_file" --gpd_init "$PAX/init_file" --gpd_init "$HERE/init_file")
canon="$WORK/canon/\$name.diff"
env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U0 "\${cb[@]}" "\$exp" "\$res" 2> /dev/null |
	perl "$HERE/../singlenode/canon.pl" > "\$canon"
st=("\${PIPESTATUS[@]}")
[ "\${st[0]}" -le 1 ] && [ "\${st[1]}" -eq 0 ] || echo "no comparison was made" >> "\$canon"
if [ ! -s "\$canon" ] || cmp -s "\$canon" "$HERE/cloudberry/\$name.diff"; then
	rm -f "\$canon"
	exit 0
fi
exec env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" "\${opts[@]}" "\${cb[@]}" "\$exp" "\$res"
EOF
chmod +x "$EXEC/bin/diff"

# A statement that runs for minutes is a finding, as the greenplum suite
# says.
TIMEOUT="${STATEMENT_TIMEOUT:-120 seconds}"
watchdog() {
	local pid query
	while :; do
		sleep 5
		PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -F ' ' -d postgres -c "
			SELECT pid, regexp_replace(left(query, 300), '\\s+', ' ', 'g')
			  FROM pg_stat_activity
			 WHERE datname = 'pax_test' AND state = 'active'
			   AND now() - query_start > interval '$TIMEOUT'" 2> /dev/null |
		while read -r pid query; do
			[ -n "$pid" ] || continue
			PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -d postgres \
				-c "SELECT pg_cancel_backend($pid)" > /dev/null 2>&1 &&
				echo "$(date +%T)  $query" >> "$1"
		done
	done
}

# pg_regress writes a test's results where its name says, statistics/ too
for t in $run_tests; do mkdir -p "$WORK/out/results/$(dirname "$t")"; done

watchdog "$WORK/cancelled" &
WATCHDOG=$!
cd "$SN"
# The database, made here rather than by pg_regress: Cloudberry's CI runs the
# schedule with default_table_access_method = pax (.github/workflows), which
# the database has once the extensions' own tables are made.
"$PSQL" -X -q -d postgres -c "CREATE DATABASE pax_test" &&
"$PSQL" -X -q -d pax_test -c "CREATE EXTENSION gp_inject_fault" \
	-c "ALTER DATABASE pax_test SET default_table_access_method = pax" ||
	{ echo "could not make the database pax_test"; exit 1; }
PATH="$EXEC/bin:$PATH" PGOPTIONS="-c gp.optimizer=off" \
	"$PG_REGRESS" \
		--bindir="$BINDIR" \
		--inputdir="$SN" \
		--expecteddir="$SN" \
		--outputdir="$WORK/out" \
		--schedule="$SN/schedule" \
		--use-existing \
		--dbname=pax_test \
		--host="$SOCK/n0" --port="$(port 0)" \
	> "$WORK/pg_regress.out" 2>&1
rc=$?
kill "$WATCHDOG" 2> /dev/null; wait "$WATCHDOG" 2> /dev/null; WATCHDOG=

grep -E "^(not )?ok " "$WORK/pg_regress.out" | sed 's/^/  /'
total=$(grep -cE "^(not )?ok " "$WORK/pg_regress.out")
bad=$(grep -cE "^not ok " "$WORK/pg_regress.out")
echo "  $((total - bad)) of $total passed"
if [ -s "$WORK/cancelled" ]; then
	echo "  cancelled after $TIMEOUT:"
	sed 's/^/    /' "$WORK/cancelled"
fi
sed -nE 's/^ok +[0-9]+ +[-+] +([^ ]+) .*/\1/p' "$WORK/pg_regress.out" |
while read -r t; do
	rm -f "$WORK/canon/$(echo "$t" | tr / _).diff"
done
unreviewed=$(cd "$WORK/canon" && ls -- *.diff 2> /dev/null | sed 's/\.diff$//')
if [ -n "$unreviewed" ]; then
	echo "  differences no one has reviewed, from the expected output named:"
	echo $unreviewed | fold -s -w 72 | sed 's/^/    /'
fi
if [ "$rc" -ne 0 ] && [ -n "${RESULTS_DIR:-}" ]; then
	cp "$WORK/out/regression.diffs" "$RESULTS_DIR/pax.diffs" 2> /dev/null
	mkdir -p "$RESULTS_DIR/pax-canon" "$RESULTS_DIR/pax-results"
	cp "$WORK"/canon/*.diff "$RESULTS_DIR/pax-canon/" 2> /dev/null
	cp -r "$WORK"/out/results/. "$RESULTS_DIR/pax-results/" 2> /dev/null
	cp "$WORK/pg_regress.out" "$RESULTS_DIR/pax-pg_regress.out" 2> /dev/null
fi

[ "$rc" -eq 0 ]
