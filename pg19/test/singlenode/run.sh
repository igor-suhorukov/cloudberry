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
# Cloudberry's singlenode regression suite, as the port runs it, against
# every M1 module, and gp_ao and gp_exttable, M5's.
#
# src/test/singlenode_regress is what Cloudberry runs in its single-node
# mode: PostgreSQL's regression tests, its parallel_schedule, and then
# Cloudberry's own, its greenplum_schedule, in one database -- the second
# half reading the tables the first leaves behind.  The port runs it the same
# way, with one substitution:
#
#   * PostgreSQL's tests are PostgreSQL 19's, with PostgreSQL 19's expected
#     output, from the commit the server was built from.  Cloudberry's copies
#     of them were made for PostgreSQL 14; on PostgreSQL 19 they would test
#     five years of PostgreSQL's changes rather than the port.
#   * Cloudberry's tests are Cloudberry's files, after all of PostgreSQL's,
#     in their own order.  manifest says of each of the 290 tests the two
#     schedules name whether it runs, is PostgreSQL's, or is skipped and why.
#     It also puts each in a group: the first group runs on the server
#     PostgreSQL's tests ran on, and each other on a copy of it made when
#     they finished, all of them side by side -- each group still reading
#     the tables PostgreSQL's tests leave, and in its own order.
#
# Two changes are made to Cloudberry's files, mechanically, to the test and
# its expected output alike: input/ and output/ .source files are converted
# as Cloudberry's pg_regress converts them, which PostgreSQL 19's no longer
# does; and a setting the port has is spelled as the port spells it --
# optimizer is gp.optimizer, and so is the column SHOW names for it --
# because PostgreSQL 19 defines no custom setting without a dot.  A setting
# the port lacks is left alone, to fail where it would.  Where the port's output still differs for a reason of its own,
# expected/ holds the port's -- an alternative for one of PostgreSQL's tests,
# and the output of the port's own setup, sql/gp_setup.sql, which runs
# between the two halves -- or orca/ and cloudberry/ a difference reviewed
# and kept (below).  Each directory's README says why each file is there.
#
# How each test is compared, in two passes over a fresh database each:
#
#                  planner (gp.optimizer = off)     orca (gp.optimizer = on)
#   PostgreSQL's   line for line, less the          gpdiff.pl, plans not
#                  Optimizer line gp_orca adds      compared (init_file_pg),
#                  to a text-format EXPLAIN         or exactly a difference
#                                                   in orca/
#   Cloudberry's   gpdiff.pl as Cloudberry's pg_regress runs it, under its
#                  init_file and the port's, plans not compared, or exactly
#                  a difference in cloudberry/
#
# The planner pass is Cloudberry's own single-node configuration -- its
# planner hook calls ORCA only as a dispatcher -- and the orca pass is the
# port's default.  In the first, PostgreSQL's tests show that the port's
# planner is PostgreSQL 19's, line for line; so a plan in one of Cloudberry's
# tests that differs from Cloudberry's is Cloudberry's planner, which the port
# does not have, and Cloudberry's tests are compared for their answers.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CB="${CB_SINGLENODE_DIR:-/cb/src/test/singlenode_regress}"
GPDIFF="${GPDIFF_DIR:-/cb/src/test/regress}"
PGSUITE="${PG_REGRESS_SUITE:-/cb/pgregress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"

if [ ! -f "$CB/greenplum_schedule" ] || [ ! -f "$PGSUITE/parallel_schedule" ] ||
   [ ! -f "$PGSUITE/regress.so" ] || [ ! -x "$PG_REGRESS" ] ||
   [ ! -f "$GPDIFF/gpdiff.pl" ]; then
	echo "a test suite, pg_regress or gpdiff.pl is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-singlenode-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbs-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts
# noexec: a diff there is skipped for the next one on PATH without a word,
# and a C module fails to map.  So the diff pg_regress runs is under the
# home directory, and the tests load regress.so from the image.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-singlenode-XXXXXX")"
PORT="${PGPORT:-$((7100 + RANDOM % 200))}"
# The superuser the cluster is made with is Cloudberry's demo cluster's,
# gpadmin, which Cloudberry's expected output names and numbers 10 (the tag
# test's owners); PostgreSQL's own tests name none.
export PGPORT="$PORT" PGHOST="$SOCK" PGUSER=gpadmin

WATCHDOGS=()
cleanup() {
	[ "${#WATCHDOGS[@]}" -gt 0 ] && kill "${WATCHDOGS[@]}" 2> /dev/null
	[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/log" "$RESULTS_DIR/singlenode-server.log" 2> /dev/null
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	for c in "$WORK"/copy*; do
		[ -d "$c/data" ] || continue
		[ -n "${RESULTS_DIR:-}" ] && cp "$c/log" "$RESULTS_DIR/singlenode-server-$(basename "$c").log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$c/data" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

run_tests=$(awk '$1 == "run" { print $2 }' "$HERE/manifest")
# The groups Cloudberry's tests run in, in the order they first appear; a
# test the manifest gives none is in the first.
groups=($(awk '$1 == "run" && $3 != "" && !seen[$3]++ { print $3 }' "$HERE/manifest"))
[ "${#groups[@]}" -gt 0 ] || groups=(1)

echo "singlenode: Cloudberry's singlenode suite, with every M1 module, gp_ao and gp_exttable loaded"
echo "  PostgreSQL's tests from $(cat "$PGSUITE/.pg_ref_commit" 2>/dev/null || echo '?'), the server from $(cat "$("$BINDIR/pg_config" --bindir)/../.pg_ref_commit" 2>/dev/null || echo '?')"
printf '  of the 290 tests Cloudberry schedules: %d of Cloudberry'"'"'s run here, %d are PostgreSQL'"'"'s, %d lines skip\n' \
	"$(echo "$run_tests" | wc -w)" \
	"$(awk '$1 == "pg"' "$HERE/manifest" | wc -l)" \
	"$(awk '$1 == "skip"' "$HERE/manifest" | wc -l)"
awk '$1 == "skip" { $1 = ""; sub(/^ /, ""); print "  skip " $0 }' "$HERE/manifest" | cut -c1-150
echo

"$BINDIR/initdb" -D "$WORK/data" -N -U gpadmin --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "fsync = off"
	# Every module there is to load here, in the order cloudberry.md gives:
	# M1's, gp_ao and gp_exttable, M5's, and gp_resource, M6's.
	echo "shared_preload_libraries = 'gp_core,gp_orca,gp_task,gp_matview,gp_sql,gp_security,gp_ao,gp_exttable,gp_resource'"
	# the settings of Cloudberry's half alone (cloudberry_half below)
	echo "include_if_exists = 'cloudberry_half.conf'"
	# What pg_regress's own temporary instance sets that a test depends on.
	echo "max_prepared_transactions = 2"
} >> "$WORK/data/postgresql.conf"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

# The settings the port has, and what respells Cloudberry's names for them
# where a statement names one (../respell.pl): SET, RESET, SHOW,
# current_setting() and set_config().  gp.optimizer* was Cloudberry's
# optimizer*, and so were the few other names Cloudberry gave no gp_
# (gp_settings.c); every other gp.x was gp_x.
{
	# SHOW's column is named for the setting, so a field of a row read from
	# it -- FOR r IN EXECUTE 'show optimizer' ... r.optimizer -- is too,
	# quoted for its dot.  First, so that the gp.optimizer SHOW becomes is
	# not taken for a field.  And last the header SHOW prints for it, in the
	# expected output: gp_x and gp.x are as wide, so the column is.
	# optimizer and gp.optimizer are not, and a SHOW of one differs, header
	# and rule.
	echo "kinds field set func header"
	"$PSQL" -X -q -t -A -d postgres -c "SELECT name FROM pg_settings WHERE name LIKE 'gp.%' ORDER BY length(name) DESC" |
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
		echo "map $cbname $name"
	done
} > "$WORK/respell"
respell() { perl "$HERE/../respell.pl" "$WORK/respell" "$@"; }

# The suite the tests run from: PostgreSQL 19's, with Cloudberry's tests
# added after its schedule, and the port's expected output where it has one.
SN="$WORK/sn"
cp -r "$PGSUITE" "$SN"

# Cloudberry builds this suite in its source tree, where @abs_srcdir@ and
# @abs_builddir@ are one directory, and its files use them interchangeably:
# singlenode_compatibility_cbdb's input loads @abs_builddir@/regress.so where
# its output says @abs_srcdir@/regress.so.  Here both are the one directory
# regress.so is in, but for a data file, which is Cloudberry's suite's own:
# uao_dml_select's COPY FROM @abs_srcdir@/data/city.data.
#
# One function of Cloudberry's regress.so is the port's cb_regress's,
# checkResourceQueueMemoryLimits(), which resource_queue_function loads from
# it: this suite's regress.so is PostgreSQL's, whose functions other tests
# need, so the one is loaded from cb_regress (the greenplum suite links its
# regress.so to cb_regress, having none of PostgreSQL's to keep).
convert() {
	sed -e "s#'@abs_builddir@/regress@DLSUFFIX@', 'checkResourceQueueMemoryLimits'#'\$libdir/cb_regress', 'checkResourceQueueMemoryLimits'#g" \
	    -e "s#'@abs_srcdir@/regress.so', 'checkResourceQueueMemoryLimits'#'\$libdir/cb_regress', 'checkResourceQueueMemoryLimits'#g" \
	    -e "s#@abs_srcdir@/data/#$CB/data/#g" \
	    -e "s#@abs_srcdir@#$PGSUITE#g" \
	    -e "s#@abs_builddir@#$PGSUITE#g" \
	    -e "s#@testtablespace@#$WORK/testtablespace#g" \
	    -e "s#@libdir@#$PGSUITE#g" \
	    -e "s#@DLSUFFIX@#.so#g" "$1"
}

cp "$SN/parallel_schedule" "$SN/schedule"
: > "$SN/cloudberry_tests"
# The port's own setup comes first: the extensions Cloudberry's tests need
# the objects of (sql/gp_setup.sql says which, and why only here).
cp "$HERE"/sql/*.sql "$SN/sql/"
echo "test: gp_setup" >> "$SN/schedule"
echo "gp_setup" > "$SN/port_tests"
# A test in a directory of Cloudberry's -- uao_compaction/basic -- runs as
# uao_compaction_basic, its files so named: PostgreSQL 19's pg_regress makes
# no directory under results/ for it.  One whose .source is in a directory
# with GENERATE_ROW_AND_COLUMN_FILES -- input/uao_dml/uao_dml_select -- is
# made twice, as Cloudberry's pg_regress makes it: uao_dml/uao_dml_select_row
# and _column, @amname@ and @aoseg@ filled in for the one or the other.
amsub() {
	if [ -n "$am" ]; then
		sed -e "s/@amname@/$am/g" -e "s/@aoseg@/$aoseg/g"
	else
		cat
	fi
}
# psql 19 skips the in-line data of a COPY ... FROM STDIN that fails, up to
# the next \. (PostgreSQL's d6ab88d374a), where psql 16, which Cloudberry's
# tests were written for, went on with the next line.  A COPY FROM STDIN a
# test expects to fail, with no data after it -- the next line a comment or
# a statement -- is given data that ends at once, which psql reads and does
# not echo, so that what the test runs next is run.
copy_data_end() {
	awk '{
		l = tolower($0)
		# the blank lines after such a COPY, until what follows them is known
		if (pending && l ~ /^[ \t]*$/) {
			blanks = blanks $0 "\n"
			next
		}
		if (pending && (l ~ /^--/ || l ~ /^[ \t]*(abort|begin|commit|copy|create|drop|end|insert|reset|rollback|select|set)([ \t;]|$)/))
			print "\\."
		printf "%s", blanks
		blanks = ""
		pending = (l ~ /^[ \t]*copy[ \t].*[ \t]from[ \t]+stdin([ \t].*)?;[ \t]*(--.*)?$/)
		print
	}
	END { printf "%s", blanks }'
}
for t in $run_tests; do
	f=$(echo "$t" | tr / _)
	src=$t am=
	for v in row:ao_row:aoseg column:ao_column:aocsseg; do
		IFS=: read -r suffix a s <<< "$v"
		if [ "${t%_$suffix}" != "$t" ] && [ -f "$CB/input/$(dirname "$t")/GENERATE_ROW_AND_COLUMN_FILES" ]; then
			src=${t%_$suffix} am=$a aoseg=$s
		fi
	done
	if [ -f "$CB/input/$src.source" ]; then
		convert "$CB/input/$src.source" | amsub | respell |
			copy_data_end > "$SN/sql/$f.sql"
	else
		respell "$CB/sql/$t.sql" | copy_data_end > "$SN/sql/$f.sql"
	fi
	if [ -f "$CB/output/$src.source" ]; then
		convert "$CB/output/$src.source" | amsub | respell > "$SN/expected/$f.out"
	else
		for e in "$CB/expected/$t.out" "$CB"/expected/"$t"_[0-9].out; do
			[ -f "$e" ] || continue
			respell "$e" > "$SN/expected/$(echo "${e#"$CB"/expected/}" | tr / _)"
		done
	fi
	g=$(awk -v t="$t" '$1 == "run" && $2 == t { print $3 }' "$HERE/manifest")
	echo "test: $f" >> "$SN/schedule.${g:-${groups[0]}}"
	echo "$f" >> "$SN/cloudberry_tests"
done
# The port's: an alternative for a test of PostgreSQL's, or the whole
# expected output of one of Cloudberry's.
[ -d "$HERE/expected" ] && cp "$HERE"/expected/*.out "$SN/expected/" 2> /dev/null
mkdir -p "$WORK/testtablespace"

mkdir -p "$WORK/gpdiff"
cp "$GPDIFF"/gpdiff.pl "$GPDIFF"/atmsort.pm "$GPDIFF"/explain.pm "$WORK/gpdiff/"
# gpdiff.pl reads its version from GPTest.pm, which Cloudberry's configure
# generates; the port has no configure.
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$GPDIFF/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# pg_regress compares with whatever "diff" is first on PATH.  This one
# chooses the comparison by whose test it is, and calls the real diff by
# its path.  Cloudberry's options are its pg_regress's own.
#
# Where gpdiff.pl compares, a test also passes if what differs is exactly a
# difference already reviewed and kept, in the form canon.pl gives it: orca/
# for PostgreSQL's tests under ORCA, cloudberry/ for Cloudberry's tests, a
# file named for the expected output it differs from, with the pass in the
# name if it is one pass's alone; and postgres/ for PostgreSQL's tests in
# the planner pass, where a statement of theirs is Cloudberry's SQL too.  Each directory's README says why each is
# there.  Any other difference fails, and is left in canon/ for review.
mkdir -p "$EXEC/bin"
cat > "$EXEC/bin/diff" <<EOF
#!/bin/bash
# pg_regress runs: diff [options] expected results
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
gpdiff() { exec env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" "\$@"; }
reviewed() {
	local dir="\$1" name canon
	shift
	name=\$(basename "\$exp" .out)
	canon="$WORK/\$CB_DIFF_MODE/canon/\$name.diff"
	env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U0 "\$@" "\$exp" "\$res" 2> /dev/null |
		perl "$HERE/canon.pl" > "\$canon"
	# A comparison that could not be made is a difference, never an empty one.
	st=("\${PIPESTATUS[@]}")
	[ "\${st[0]}" -le 1 ] && [ "\${st[1]}" -eq 0 ] || echo "no comparison was made" >> "\$canon"
	if [ ! -s "\$canon" ] || cmp -s "\$canon" "\$dir/\$name.\$CB_DIFF_MODE.diff" ||
	   cmp -s "\$canon" "\$dir/\$name.diff"; then
		rm -f "\$canon"
		exit 0
	fi
}
if grep -qxF "\$(basename "\$res" .out)" "$SN/cloudberry_tests"; then
	cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
	    --gpd_init "$CB/init_file" --gpd_init "$HERE/init_file")
	# less the place PostgreSQL 19 gives a shell type (shellpos.pl)
	mkdir -p "$WORK/\$CB_DIFF_MODE/shellpos"
	perl "$HERE/shellpos.pl" < "\$res" > "$WORK/\$CB_DIFF_MODE/shellpos/\$(basename "\$res")"
	res="$WORK/\$CB_DIFF_MODE/shellpos/\$(basename "\$res")"
	reviewed "$HERE/cloudberry" "\${cb[@]}"
	gpdiff "\${opts[@]}" "\${cb[@]}" "\$exp" "\$res"
fi
# PostgreSQL's tests, less the line gp_orca adds to EXPLAIN in both passes:
# gpdiff.pl passes over it in a plan, but not in EXPLAIN a function returns
# as rows, whose count it is in.
stripped="$WORK/\$CB_DIFF_MODE/stripped/\$(basename "\$res")"
mkdir -p "\$(dirname "\$stripped")"
perl "$HERE/strip_optimizer.pl" "\$res" > "\$stripped"
res="\$stripped"
if [ "\$CB_DIFF_MODE" = orca ]; then
	pg=(-I GP_IGNORE: --gpd_ignore_plans --gpd_init "$HERE/init_file_pg")
	reviewed "$HERE/orca" "\${pg[@]}"
	gpdiff "\${opts[@]}" "\${pg[@]}" "\$exp" "\$res"
fi
# In the planner pass, line for line -- but for a test of PostgreSQL's that
# meets Cloudberry's SQL, which the port speaks, where postgres/ holds the
# difference, reviewed and kept.
[ -f "$HERE/postgres/\$(basename "\$exp" .out).diff" ] && reviewed "$HERE/postgres"
exec /usr/bin/diff "\${opts[@]}" "\$exp" "\$res"
EOF
chmod +x "$EXEC/bin/diff"

# A statement that runs for minutes is a finding, not something to wait for:
# ORCA can plan as nested SubPlans a query the planner turns into joins, and
# such a plan does not finish.  statement_timeout would say so, but some tests
# print it, so a watchdog cancels such a statement instead, and says which.
#
# The statements are found first and cancelled one by one afterwards: a
# query that cancels in its WHERE clause cancels whatever its quals are
# evaluated against first, which is not only the rows the rest of it keeps.
#
# One statement is known not to finish in the orca pass, and is cancelled
# there as soon as the watchdog sees it, rather than after the minute:
# subselect's doubly correlated EXISTS over NOT EXISTS, which Cloudberry's own
# subselect test runs with the planner (orca/README).  It is found by its
# innermost subquery, which no other statement of the suite has.
TIMEOUT="${STATEMENT_TIMEOUT:-60 seconds}"
HANGS='not exists \( select 1 from tenk1 d\s+where a\.thousand = d\.thousand \)'
watchdog() {					# watchdog <file of what it cancelled> <pass>
	local pid query hang=
	[ "$2" = orca ] &&
		hang="OR (query ~ '$HANGS' AND now() - query_start > interval '2 seconds')"
	while :; do
		sleep 2
		PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -F ' ' -d postgres -c "
			SELECT pid, regexp_replace(left(query, 300), '\\s+', ' ', 'g')
			  FROM pg_stat_activity
			 WHERE datname = 'regression' AND state = 'active'
			   AND (now() - query_start > interval '$TIMEOUT' $hang)" 2> /dev/null |
		while read -r pid query; do
			[ -n "$pid" ] || continue
			PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -d postgres \
				-c "SELECT pg_cancel_backend($pid)" > /dev/null 2>&1 &&
				echo "$(date +%T)  $query" >> "$1"
		done
	done
}

# A server for each group of Cloudberry's tests but the first, which runs on
# this one: a copy of its data directory, made while it is down, as
# PostgreSQL's tests and the port's setup left it, on a socket of its own.
# A tablespace outside the data directory would be the copies' to share, so
# there must be none; PostgreSQL 19's tests make theirs inside it.
copy_data() {
	local c="$WORK/copy$1"

	rm -rf "$c"
	mkdir -p "$c" "$SOCK/copy$1"
	cp -a "$WORK/data" "$c/data" || return 1
	{
		echo "unix_socket_directories = '$SOCK/copy$1'"
		echo "port = $PORT"
	} >> "$c/data/postgresql.conf"
}
start_copy() {
	"$BINDIR/pg_ctl" -D "$WORK/copy$1/data" -l "$WORK/copy$1/log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "  a copy of the server did not start"; tail -20 "$WORK/copy$1/log"; return 1; }
}

# One pg_regress: its schedule, its output directory, its server.  What it
# says goes to <outputdir>/pg_regress.out.
regress() {
	local schedule="$1" out="$2" host="$3"
	shift 3

	mkdir -p "$out"
	# From Cloudberry's suite's directory, as its Makefile runs it: one of its
	# tests, partition_indexing, reads data/onek.data there by a relative
	# path.  PostgreSQL 19's tests name their files by absolute path.  The
	# expected outputs are named, because pg_regress looks for them in
	# expected/ of the directory it runs in before the one --inputdir names,
	# and Cloudberry's directory has its copies of PostgreSQL 14's.
	( cd "$CB" &&
	  PATH="$EXEC/bin:$PATH" CB_DIFF_MODE="$pass" PGOPTIONS="-c gp.optimizer=$optimizer" \
		"$PG_REGRESS" \
			--bindir="$BINDIR" \
			--inputdir="$SN" \
			--expecteddir="$SN" \
			--outputdir="$out" \
			--dlpath="$PGSUITE" \
			--schedule="$schedule" \
			--max-connections=20 \
			--host="$host" --port="$PORT" \
			"$@" \
		> "$out/pg_regress.out" 2>&1 )
}

# What Cloudberry's half runs with and PostgreSQL's does not: Cloudberry's
# memory policy, as its postgresql.conf.sample sets it, under which a
# statement's memory is its resource queue's to give -- gp_resource's
# budget, which a query runs with as its work_mem, where PostgreSQL's tests
# set work_mem themselves and read it back.  The file postgresql.conf
# includes: written as the server stops to be copied for Cloudberry's half,
# and emptied, and the server told, before PostgreSQL's half of each pass.
cloudberry_half() {
	if [ "$1" = on ]; then
		echo "gp.resqueue_memory_policy = 'eager_free'" > "$WORK/data/cloudberry_half.conf"
	else
		: > "$WORK/data/cloudberry_half.conf"
		"$BINDIR/pg_ctl" -D "$WORK/data" reload > /dev/null 2>&1
		for _ in $(seq 1 50); do
			[ "$("$PSQL" -X -q -t -A -h "$SOCK" -p "$PORT" -d postgres \
				  -c "SHOW gp.resqueue_memory_policy" 2> /dev/null)" = none ] && break
			sleep 0.1
		done
	fi
}

failed=0
for pass in ${PASSES:-planner orca}; do
	echo "== pass: $pass"
	cloudberry_half off
	mkdir -p "$WORK/$pass/canon"
	case "$pass" in
		planner) optimizer=off ;;
		orca)    optimizer=on ;;
	esac

	# PostgreSQL's tests and the port's setup, in the database pg_regress
	# makes afresh.
	watchdog "$WORK/$pass/cancelled" "$pass" &
	WATCHDOGS=($!)
	regress "$SN/schedule" "$WORK/$pass/pg" "$SOCK"
	rc=$?
	kill "${WATCHDOGS[@]}" 2> /dev/null; wait "${WATCHDOGS[@]}" 2> /dev/null; WATCHDOGS=()

	# Then Cloudberry's, a group a server, side by side, each in the database
	# PostgreSQL's tests left.
	outs=("$WORK/$pass/pg")
	"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w stop > /dev/null 2>&1 \
		|| { echo "  the server did not stop to be copied"; exit 1; }
	if [ -n "$(find "$WORK/data/pg_tblspc" -mindepth 1 -type l 2> /dev/null)" ]; then
		echo "  the server has a tablespace outside its data directory, which its copies would share"
		exit 1
	fi
	cloudberry_half on
	copies=()
	for i in $(seq 1 $((${#groups[@]} - 1))); do
		copy_data "$i" &
		copies+=($!)
	done
	for p in "${copies[@]}"; do
		wait "$p" || { echo "  the server could not be copied"; exit 1; }
	done
	copies=()
	for i in $(seq 1 $((${#groups[@]} - 1))); do
		start_copy "$i" &
		copies+=($!)
	done
	"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "  the server did not start again"; tail -20 "$WORK/log"; exit 1; }
	for p in "${copies[@]}"; do
		wait "$p" || exit 1
	done
	pids=()
	for i in "${!groups[@]}"; do
		if [ "$i" -eq 0 ]; then host="$SOCK"; else host="$SOCK/copy$i"; fi
		PGHOST="$host" watchdog "$WORK/$pass/cancelled" "$pass" &
		WATCHDOGS+=($!)
		regress "$SN/schedule.${groups[$i]}" "$WORK/$pass/${groups[$i]}" "$host" --use-existing &
		pids+=($!)
		outs+=("$WORK/$pass/${groups[$i]}")
	done
	for p in "${pids[@]}"; do
		wait "$p" || rc=1
	done
	kill "${WATCHDOGS[@]}" 2> /dev/null; wait "${WATCHDOGS[@]}" 2> /dev/null; WATCHDOGS=()
	for i in $(seq 1 $((${#groups[@]} - 1))); do
		"$BINDIR/pg_ctl" -D "$WORK/copy$i/data" -m immediate stop > /dev/null 2>&1
	done

	# What they said, as one run's: in order, and numbered as one.
	mkdir -p "$WORK/$pass/results"
	: > "$WORK/$pass/pg_regress.out"
	for o in "${outs[@]}"; do
		cat "$o/pg_regress.out" >> "$WORK/$pass/pg_regress.out"
		cat "$o/regression.diffs" >> "$WORK/$pass/regression.diffs" 2> /dev/null
		cp "$o"/results/*.out "$WORK/$pass/results/" 2> /dev/null
	done
	awk '/^(not )?ok / { n++; sub(/^(not )?ok +[0-9]+ +/, sprintf("%s %-9d ", ($1 == "not" ? "not ok" : "ok"), n)) } { print }' \
		"$WORK/$pass/pg_regress.out" > "$WORK/$pass/pg_regress.out.n" &&
		mv "$WORK/$pass/pg_regress.out.n" "$WORK/$pass/pg_regress.out"

	grep -E "^(not )?ok " "$WORK/$pass/pg_regress.out" | sed 's/^/  /'
	for whose in PostgreSQL Cloudberry port; do
		total=0; bad=0
		while read -r status name; do
			if grep -qxF "$name" "$SN/cloudberry_tests"; then mine=Cloudberry
			elif grep -qxF "$name" "$SN/port_tests"; then mine=port
			else mine=PostgreSQL; fi
			[ "$mine" = "$whose" ] || continue
			total=$((total + 1))
			[ "$status" = "not" ] && bad=$((bad + 1))
		done < <(sed -nE 's/^(not )?ok +[0-9]+ +[-+] +([^ ]+) .*/\1 \2/p' "$WORK/$pass/pg_regress.out" |
		         sed -E 's/^not /not /; s/^ /ok /')
		[ "$whose" = port ] && who="The port's own setup" || who="$whose's tests"
		echo "  $who: $((total - bad)) of $total passed"
	done
	if [ -s "$WORK/$pass/cancelled" ]; then
		echo "  cancelled after $TIMEOUT, or as soon as seen where known not to finish:"
		sed 's/^/    /' "$WORK/$pass/cancelled"
	fi
	# A test that passed on an alternative expected output left a difference
	# from the first one it was compared with, which is no finding.
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
			# What Cloudberry's tests were run from, converted and respelled,
			# for a diff to be read against.
			mkdir -p "$RESULTS_DIR/singlenode-inputs/sql" "$RESULTS_DIR/singlenode-inputs/expected"
			while read -r t; do
				cp "$SN/sql/$t.sql" "$RESULTS_DIR/singlenode-inputs/sql/" 2> /dev/null
				cp "$SN/expected/$t".out "$SN/expected/$t"_[0-9].out "$RESULTS_DIR/singlenode-inputs/expected/" 2> /dev/null
			done < "$SN/cloudberry_tests"
			cp "$WORK/respell" "$RESULTS_DIR/singlenode-inputs/"
			cp "$WORK/$pass/regression.diffs" "$RESULTS_DIR/singlenode-$pass.diffs" 2> /dev/null
			mkdir -p "$RESULTS_DIR/singlenode-$pass-canon"
			cp "$WORK/$pass"/canon/*.diff "$RESULTS_DIR/singlenode-$pass-canon/" 2> /dev/null
			mkdir -p "$RESULTS_DIR/singlenode-$pass-results"
			cp "$WORK/$pass"/results/*.out "$RESULTS_DIR/singlenode-$pass-results/" 2> /dev/null
		fi
	fi
	echo
done

[ "$failed" -eq 0 ]
