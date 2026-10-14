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
# M1 module, M5's gp_ao and gp_exttable, and M6's gp_resource.
#
# src/test/singlenode_isolation2 is the tests Cloudberry runs in its
# single-node mode that need more than one session at a time, written in its
# isolation2 syntax (1: ..., 2&: ..., 2<:) and run by its own driver,
# sql_isolation_testcase.py, over PyGreSQL.  The port runs the driver as
# Cloudberry's pg_isolation2_regress does -- the test on its standard input,
# the result on its standard output -- and compares the result with gpdiff.pl
# under Cloudberry's init files, as Cloudberry's pg_regress does.  The one
# change is the setup: setup.sql here, in place of Cloudberry's, says why.
# The other is the isolation2 suite's: a setting the port has is spelled as
# the port spells it, in the tests and in their expected output alike, and a
# shell command of a test finds gpMgmt's gpconfig and gpstop, set up as
# ../gpmgmt/tools.sh sets them up, which do on this node, the port's single
# node, what the tests ask of Cloudberry's.  They connect to template1, which
# has gp_core too, and the tests' database is made from template0, as it was
# made before template1 had it.  And the driver's retrieve sessions reach the
# node by its socket (see below), for the port's own test of parallel
# retrieve cursors (port/, M8), in place of Cloudberry's schedule of them.
# A session of the node's own (-1U:) is the node's connection, with no
# setting to say so.
#
# A test of the schedule's uao/ directory is one of two that Cloudberry's
# pg_regress makes of one file, input/uao/x.source: uao/x_row, of a row
# table, and uao/x_column, of a column table, each compared with what
# output/uao/x.source becomes for it -- as the isolation2 suite makes them on
# a cluster, where most of these files run too, on three segments.
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
# Another schedule runs through this harness too, with a manifest of its
# own: ../singlenode_resgroup's, the resource group tests on one node, as
# ../resgroup's run through the isolation2 suite's harness on a cluster.
# What it gives: its name, in what is printed, and what its manifest lists
# (SN_ISOLATION2_WHAT); the manifest; the directory of its differences kept
# -- or several, the first first, for a schedule whose tests are another's
# too -- and the one of its own tests (port/); the directory of Cloudberry's
# its tests are read from, which run from this suite's as the driver does, a
# test that directory lacks being this suite's; the database it runs in; the
# init file of Cloudberry's it is compared under, and init files of its own
# beside this suite's; the kinds of respelling its tests want
# (../respell.pl), and rules of sed of its own, applied as the respelling
# is, to each test and its expected output alike.
SUITE="${SN_ISOLATION2_SUITE:-singlenode_isolation2}"
MANIFEST="${SN_ISOLATION2_MANIFEST:-$HERE/manifest}"
WHAT="Cloudberry's singlenode isolation2 suite, with every M1 module, gp_ao, gp_exttable and gp_resource loaded"
WHAT="${SN_ISOLATION2_WHAT:-$WHAT}"
KEPT="${SN_ISOLATION2_KEPT:-$HERE/cloudberry}"
OWN="${SN_ISOLATION2_OWN:-$HERE}"
TESTS="${SN_ISOLATION2_TESTS_DIR:-$CB}"
DBNAME="${SN_ISOLATION2_DBNAME:-isolation2test}"
CB_INIT="${SN_ISOLATION2_CB_INIT:-$CB/init_file_isolation2}"
EXTRA_INIT="${SN_ISOLATION2_EXTRA_INIT:-}"
KINDS="${SN_ISOLATION2_KINDS:-setsys altersys func nameeq header0}"
EXTRA_SED="${SN_ISOLATION2_EXTRA_SED:-}"
# ... and the node's superuser, where its tests name one: the isolation2
# suite's, gpadmin, for tests that are that suite's
if [ -n "${SN_ISOLATION2_SUPERUSER:-}" ]; then
	export PGUSER="$SN_ISOLATION2_SUPERUSER"
fi

if [ ! -f "$CB/sql_isolation_testcase.py" ] || [ ! -f "$GPDIFF/gpdiff.pl" ] ||
   [ ! -f "$GPDIFF/init_file" ] || ! python3 -c 'import pg' 2> /dev/null; then
	echo "Cloudberry's isolation2 suite, gpdiff.pl or PyGreSQL is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-isolation2-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbi-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-snisolation2-XXXXXX")"
if ! . "$HERE/../gpmgmt/tools.sh" "$EXEC"; then
	echo "gpMgmt, or the Python it needs, is not installed; skipping"
	rm -rf "$WORK" "$SOCK" "$EXEC"
	exit 77
fi
PORT="${PGPORT:-$((7700 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK" COORDINATOR_DATA_DIRECTORY="$WORK/data"

cleanup() {
	if [ -n "${RESULTS_DIR:-}" ]; then
		cp "$WORK/log" "$RESULTS_DIR/isolation2-server.log" 2> /dev/null
		# ... and the log of a start of gpstart's
		cp "$WORK/data/log/startup.log" "$RESULTS_DIR/isolation2-server.startup.log" 2> /dev/null
	fi
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

run_tests=$(awk '$1 == "run" { print $2 }' "$MANIFEST")

echo "$SUITE: $WHAT"
printf '  of the %d tests Cloudberry schedules: %d run here, %d are skipped\n' \
	"$(grep -cE '^(run|skip) ' "$MANIFEST")" \
	"$(echo "$run_tests" | wc -w)" \
	"$(grep -c '^skip ' "$MANIFEST")"
awk '$1 == "skip" { $1 = ""; sub(/^ /, ""); print "  skip " $0 }' "$MANIFEST" | cut -c1-150
echo

"$BINDIR/initdb" -D "$WORK/data" -N ${SN_ISOLATION2_SUPERUSER:+-U "$SN_ISOLATION2_SUPERUSER"} --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "fsync = off"
	echo "shared_preload_libraries = 'gp_core,gp_orca,gp_task,gp_matview,gp_sql,gp_security,gp_ao,gp_exttable,gp_resource'"
	# the connections of Cloudberry's demo cluster's coordinator
	# (gpdemo-defaults.sh), where a test opens more sessions at once than
	# PostgreSQL's hundred: one for each segment file an append-optimized
	# table may have (uao/max_concurrency)
	echo "max_connections = 150"
	# as Cloudberry's postgresql.conf.sample: a statement's memory is its
	# resource queue's to give
	echo "gp.resqueue_memory_policy = 'eager_free'"
} >> "$WORK/data/postgresql.conf"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }
# gp_core where gpMgmt's tools ask about the node, and where the driver does
# as a test opens a retrieve session (-1R:), in postgres
for db in template1 postgres; do
	"$PSQL" -X -q -d "$db" -c "CREATE EXTENSION gp_core" > /dev/null ||
		{ echo "could not create gp_core in $db"; exit 1; }
done
# ... and, for a schedule that asks it, an extension more in postgres: a
# helper of a resource group test's creates a group there, by psql
for ext in ${SN_ISOLATION2_POSTGRES_MORE:-}; do
	"$PSQL" -X -q -d postgres -c "CREATE EXTENSION $ext" > /dev/null ||
		{ echo "could not create $ext in postgres"; exit 1; }
done

# The settings the port has, respelled as the isolation2 suite respells them
# (see there, and ../respell.pl).
{
	echo "kinds $KINDS"
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
			enable_offload_entry_to_qe|debug_dtm_action*|debug_abort_after_distributed_prepared|\
			debug_appendonly_print_*|\
			debug_print_full_dtm|enable_answer_query_using_materialized_views|aqumv_allow_foreign_table|\
			allow_segment_dml|pljava_classpath|pljava_vmoptions)
				cbname="$short" ;;
			*) cbname="gp_$short" ;;
		esac
		echo "map $cbname $name"
	done
	# A PL/Python block that leaves its backend in /tmp, so that what it
	# writes is not in the node's data directory (packcore's, which writes
	# nothing where no core file is): PostgreSQL finds a relation's file by
	# a path from its own directory, and a file that backend opens after is
	# not found, "could not open file".  The backend stays where it is, as
	# in the isolation2 suite.
	echo "sed s/os\\.chdir\\('\\/tmp'\\)/pass/"
	[ -n "$EXTRA_SED" ] && sed 's/^/sed /' "$EXTRA_SED"
} > "$WORK/respell"
respell() { perl "$HERE/../respell.pl" "$WORK/respell" "$@"; }
export PG_BINDIR="$BINDIR"

mkdir -p "$WORK/gpdiff"
cp "$GPDIFF"/gpdiff.pl "$GPDIFF"/atmsort.pm "$GPDIFF"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$GPDIFF/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# The driver, run from Cloudberry's directory, as it sources
# global_sh_executor.sh from there, but for changes to a copy of it.  A
# session of the node's own (-1U:) is opened without "-c gp_role=utility",
# which PostgreSQL 19 has no such setting for, and which a connection to the
# one node is here anyway, as in the isolation2 suite.  And two for a
# retrieve session (-1R:): it finds the node among nodes of which none is
# a segment, where it divided by their count; and reaches it by its socket, as
# a session of the node's own does, where gp_segment_configuration names this
# machine -- the one node lists itself by its host name, and listens on no
# TCP port here.
sed -e 's/given_opt="-c gp_role=utility"/given_opt=None/' \
	-e 's/real_content_id = content_id % max_content_id if content_id >= 0 else/real_content_id = content_id if max_content_id == 0 else content_id % max_content_id if content_id >= 0 else/' \
	-e '/elif self.mode == "retrieve":/{n;s/$/\n                if hostname == socket.gethostname():\n                    hostname = None/}' \
	"$CB/sql_isolation_testcase.py" > "$EXEC/sql_isolation_testcase.py"

# Cloudberry's pg_regress compares with these; gpdiff.pl runs diff from PATH.
gpdiff() {
	env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" \
		-I HINT: -I CONTEXT: -I GP_IGNORE: "$@"
}

# A test's file of Cloudberry's pg_regress's making, input/x.source, as the
# isolation2 suite converts one.
convert() {
	sed -e "s#@abs_srcdir@/data/#$CB/../regress/data/#g" \
	    -e "s#@abs_srcdir@#$CB#g" \
	    -e "s#@abs_builddir@#$CB#g" \
	    -e "s#@testtablespace@#/tmp/testtablespace#g" \
	    -e "s#@bindir@#$BINDIR#g" \
	    -e "s#@libdir@#${PG_REGRESS_SUITE:-/cb/pgregress}#g" \
	    -e "s#@curusername@#${PGUSER:-$(id -un)}#g" \
	    -e "s#@hostname@#localhost#g" \
	    -e "s#@DLSUFFIX@#.so#g" "$1"
}
# A row table's or a column table's, in a file of both.
amsub() {
	if [ -n "$am" ]; then
		sed -e "s/@amname@/$am/g" -e "s/@aoseg@/$aoseg/g"
	else
		cat
	fi
}

failed=0
for pass in ${PASSES:-planner orca}; do
	echo "== pass: $pass"
	R="$WORK/$pass"
	mkdir -p "$R/results" "$R/canon" "$R/sql" "$R/expected"
	case "$pass" in
		planner) optimizer=off ;;
		orca)    optimizer=on ;;
	esac

	"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DBNAME" > /dev/null 2>&1
	"$PSQL" -X -q -d postgres -c "CREATE DATABASE $DBNAME TEMPLATE template0" > /dev/null
	if ! out=$("$PSQL" -X -q -v ON_ERROR_STOP=1 -d "$DBNAME" -f "$HERE/setup.sql" 2>&1); then
		echo "  the setup failed:"; printf '%s\n' "$out" | sed 's/^/    /'
		failed=$((failed + 1)); continue
	fi

	total=0; bad=0
	for t in $run_tests; do
		total=$((total + 1))
		res="$R/results/$t.out"
		mkdir -p "$(dirname "$res")" "$(dirname "$R/canon/$t")" \
			"$(dirname "$R/sql/$t")" "$(dirname "$R/expected/$t")"
		# A test whose name begins "port/" is the port's own, from sql/port
		# and expected/port beside this file, as Cloudberry's are from its
		# suite's -- or, for a schedule whose tests are read from another
		# directory of Cloudberry's, from that one, a test it lacks being
		# this suite's.
		src="$TESTS"
		[ ! -f "$TESTS/sql/$t.sql" ] && [ ! -f "$TESTS/input/$t.source" ] && src="$CB"
		[[ "$t" == port/* ]] && src="$OWN"
		# A test Cloudberry's pg_regress makes twice of one file, where its
		# directory says so (GENERATE_ROW_AND_COLUMN_FILES): uao/x_row and
		# uao/x_column of input/uao/x.source, an append-optimized table's
		# test of a row table and of a column table, as the isolation2 suite
		# makes them.
		tmpl="$t" am= aoseg=
		for v in row:ao_row:aoseg column:ao_column:aocsseg; do
			IFS=: read -r suffix a sg <<< "$v"
			if [ "${t%_$suffix}" != "$t" ] &&
			   [ -f "$src/input/$(dirname "$t")/GENERATE_ROW_AND_COLUMN_FILES" ]; then
				tmpl="${t%_$suffix}" am="$a" aoseg="$sg"
			fi
		done
		if [ -f "$src/input/$tmpl.source" ]; then
			convert "$src/input/$tmpl.source" | amsub | respell > "$R/sql/$t.sql"
		else
			respell "$src/sql/$t.sql" > "$R/sql/$t.sql"
		fi

		# As pg_isolation2_regress runs it, from the suite's directory.
		t0=$(date +%s%N)
		( cd "$CB" && PGOPTIONS="-c gp.optimizer=$optimizer" \
			timeout "${ISOLATION2_TEST_TIMEOUT:-300}" python3 "$EXEC/sql_isolation_testcase.py" \
				--dbname="$DBNAME" --initfile_prefix="$res" \
				< "$R/sql/$t.sql" > "$res" 2>&1 )
		ms=$(( ($(date +%s%N) - t0) / 1000000 ))

		# What it is compared with, and the name a difference kept is filed
		# under: the expected output's, which for such a test of two is its
		# own, x_row or x_column.
		cbexp="$src/expected/$t.out"
		[ "$pass" = orca ] && [ -f "$src/expected/${t}_optimizer.out" ] && cbexp="$src/expected/${t}_optimizer.out"
		[ -f "$src/output/$tmpl.source" ] && cbexp="$src/output/$tmpl.source"
		name="$(basename "$cbexp" .out)"
		name="${name%.source}"
		[ -n "$am" ] && name="$(basename "$t")"
		exp="$R/expected/$t.out"
		mkdir -p "$(dirname "$exp")"
		if [ -f "$src/output/$tmpl.source" ]; then
			convert "$cbexp" | amsub | respell > "$exp"
		else
			respell "$cbexp" > "$exp"
		fi
		inits=(--gpd_init "$GPDIFF/init_file" --gpd_init "$CB_INIT"
		       --gpd_init "$HERE/init_file")
		for init in $EXTRA_INIT; do inits+=(--gpd_init "$init"); done
		[ -s "$res.ini" ] && inits+=(--gpd_init "$res.ini")

		gpdiff -U0 "${inits[@]}" "$exp" "$res" 2> /dev/null | perl "$HERE/../singlenode/canon.pl" > "$R/canon/$t.diff"
		# A comparison that could not be made is a difference, never an empty one.
		st=("${PIPESTATUS[@]}")
		[ "${st[0]}" -le 1 ] && [ "${st[1]}" -eq 0 ] || echo "no comparison was made" >> "$R/canon/$t.diff"
		kept=0
		for dir in $KEPT; do
			dir="$dir/$(dirname "$t")"
			if cmp -s "$R/canon/$t.diff" "$dir/$name.$pass.diff" ||
			   cmp -s "$R/canon/$t.diff" "$dir/$name.diff"; then
				kept=1
			fi
		done
		if [ ! -s "$R/canon/$t.diff" ] || [ "$kept" -eq 1 ]; then
			rm -f "$R/canon/$t.diff"
			printf '  ok     %-56s %7s ms\n' "$t" "$ms"
		else
			bad=$((bad + 1))
			printf '  NOT OK %-56s %7s ms\n' "$t" "$ms"
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
