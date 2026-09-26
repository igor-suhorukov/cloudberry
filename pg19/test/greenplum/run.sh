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
if ! . "$HERE/../gpmgmt/tools.sh" "$EXEC"; then
	echo "gpMgmt, or the Python it needs, is not installed; skipping"
	rm -rf "$WORK" "$SOCK" "$EXEC"
	exit 77
fi
BASEPORT="${PGPORT:-$((7300 + RANDOM % 200))}"
NODES=4					# a coordinator and Cloudberry's three segments
PRELOAD='gp_core,gp_orca,gp_sql,gp_ao,gp_exttable,gp_security,gp_resource'
SECRET="greenplum-schedule-$RANDOM$RANDOM$RANDOM"

# The tests the manifest runs -- Cloudberry's, and the port's (port:name)
# among them where it puts them -- in its order, and the group each is in;
# the groups, in the order they first appear.
run_tests=(); run_group=()
while read -r kind t g; do
	[ "$kind" = port ] && t="port:$t"
	run_tests+=("$t"); run_group+=("$g")
done < <(awk '$1 == "group" { g = $2 }
			  $1 == "run" || $1 == "port" { print $1, $2, (g == "" ? "main" : g) }' "$HERE/manifest")
groups=()
for g in "${run_group[@]}"; do
	[[ " ${groups[*]} " == *" $g "* ]] || groups+=("$g")
done

# Node n of the gi-th group's cluster: its data directory, its port and the
# socket directory it listens on alone.
node_dir()  { echo "$WORK/$1/node$2"; }
node_port() { echo $((BASEPORT + $1 * NODES + $2)); }
node_sock() { echo "$SOCK/$1/n$2"; }
# The superuser is Cloudberry's demo cluster's, gpadmin, as the singlenode
# suite's is: Cloudberry's expected output names it.
export PGUSER=gpadmin

cleanup() {
	local w g n
	for w in $(jobs -p); do kill "$w" 2> /dev/null; done
	for g in "${groups[@]}"; do
		for n in $(seq 0 $((NODES - 1))); do
			[ -n "${RESULTS_DIR:-}" ] &&
				cp "$(node_dir "$g" "$n").log" "$RESULTS_DIR/greenplum-$g-node$n.log" 2> /dev/null
			"$BINDIR/pg_ctl" -D "$(node_dir "$g" "$n")" -m immediate stop > /dev/null 2>&1
		done
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

echo "greenplum: part of Cloudberry's greenplum_schedule, on a coordinator and three segments"
printf '  of the %d tests of the schedule the manifest lists: %d run here, in %d groups, %d are skipped\n' \
	"$(awk '$1 == "run" || $1 == "skip"' "$HERE/manifest" | wc -l)" \
	"$(awk '$1 == "run"' "$HERE/manifest" | wc -l)" "${#groups[@]}" \
	"$(awk '$1 == "skip"' "$HERE/manifest" | wc -l)"
echo

# A group's cluster, as run.sh of the cluster suite makes one.
make_cluster() {
	local g="$1" gi="$2" conf="$WORK/$1/gp_cluster.conf" n d
	mkdir -p "$WORK/$g"
	{
		echo "# dbid content role host port datadir"
		for n in $(seq 0 $((NODES - 1))); do
			echo "$((n + 1)) $((n - 1)) p $(node_sock "$g" "$n") $(node_port "$gi" "$n") $(node_dir "$g" "$n")"
		done
	} > "$conf"
	for n in $(seq 0 $((NODES - 1))); do
		d="$(node_dir "$g" "$n")"
		mkdir -p "$(node_sock "$g" "$n")"
		"$BINDIR/initdb" -D "$d" -N -U gpadmin --locale=C --encoding=UTF8 > "$d.initdb.log" 2>&1 \
			|| { echo "initdb failed for node $n of group $g"; tail -20 "$d.initdb.log"; return 1; }
		{
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "unix_socket_directories = '$(node_sock "$g" "$n")'"
			echo "listen_addresses = ''"
			echo "port = $(node_port "$gi" "$n")"
			echo "fsync = off"
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
			# each statement ORCA would not plan, and why, in the log: the
			# ORCA pass's reasons, totalled below
			[ "$n" -eq 0 ] && echo "gp.optimizer_log_fallback = on"
		} >> "$d/postgresql.auto.conf"
	done
	for n in $(seq 1 $((NODES - 1))) 0; do
		d="$(node_dir "$g" "$n")"
		"$BINDIR/pg_ctl" -D "$d" -l "$d.log" -w -t 60 start > /dev/null 2>&1 \
			|| { echo "node $n of group $g did not start"; tail -20 "$d.log"; return 1; }
	done
}
t0=$(date +%s)
for gi in "${!groups[@]}"; do
	make_cluster "${groups[$gi]}" "$gi" &
done
for g in "${groups[@]}"; do
	wait -n || exit 1
done
t1=$(date +%s)

# The settings the port has, and what respells Cloudberry's names for them
# (../respell.pl); the singlenode suite says how.
{
	# the column SHOW names, read as a row's field; SET, RESET and SHOW;
	# current_setting() and set_config(); and SHOW's header, the same width
	# spelled either way (see the singlenode suite)
	echo "kinds field set func header"
	PGHOST="$(node_sock "${groups[0]}" 0)" PGPORT="$(node_port 0 0)" \
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
	# And a program of Cloudberry's suite that a test runs from the suite's
	# directory, ./extended_protocol_resqueue, is the one the port builds and
	# installs (meson's hook_tests), run from PATH as the diff is.
	echo 'sed s#^[\\]! \./(extended_protocol_resqueue) #\\! \1 #'
	# So is bb_memory_quota's script, $PG_ABS_BUILDDIR/mem_quota_util.py, from
	# PATH (below); it runs its queries in the database it is named, which is
	# regression here.
	echo 'sed s#^[\\]! \$PG_ABS_BUILDDIR/(mem_quota_util\.py) (.*)--dbname=regress #\\! \1 \2--dbname=regression #'
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
	# a test loads regress.so from PG_ABS_SRCDIR too, the suite's directory
	ln -sf "$("$BINDIR/pg_config" --pkglibdir)/cb_regress.so" "$SN/regress.so"
	{ echo "test: test_setup"; echo "test: gp_setup"; } > "$SN/schedule"
	for i in "${!run_tests[@]}"; do
		[ "${run_group[$i]}" = "$g" ] || continue
		t="${run_tests[$i]}"
		case "$t" in
			port:*)
				echo "test: ${t#port:}" >> "$SN/schedule"
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
			convert "$g" "$CB/input/$src.source" | amsub | respell > "$SN/sql/$f.sql"
		else
			convert "$g" "$CB/sql/$t.sql" | respell > "$SN/sql/$f.sql"
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
		echo "test: $f" >> "$SN/schedule"
	done
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
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

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
    --gpd_init "$CB/init_file" --gpd_init "$HERE/init_file")
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
	# ORCA's counts, from the pass's start (gp_orca.fallbacks()), and where
	# the coordinator's log was then
	"$PSQL" -X -q -d template1 -c "SELECT gp_orca.reset_fallbacks()" > /dev/null 2>&1
	logpos=$(stat -c %s "$(node_dir "$g" 0).log")
	watchdog "$R/cancelled" &
	wd=$!
	trap 'kill "$wd" 2> /dev/null; exit 1' TERM INT
	# From Cloudberry's suite's directory, as its Makefile runs it: its tests
	# read data/ by relative paths.
	cd "$CB"
	# PG_HOSTNAME and PG_BINDDIR are what Cloudberry's pg_regress sets for
	# its tests: the host of segment 0, for their file:// and gpfdist://
	# locations -- here every node's is this one -- and the directory of
	# the programs, where they start gpfdist from.
	PATH="$EXEC/bin:$PATH" CB_DIFF_MODE="$pass" CB_DIFF_DIR="$R" \
	PGOPTIONS="-c gp.optimizer=$optimizer" \
	PG_HOSTNAME=localhost PG_BINDDIR="$BINDIR" \
	PG_BINDIR="$BINDIR" COORDINATOR_DATA_DIRECTORY="$(node_dir "$g" 0)" \
		"$PG_REGRESS" \
			--bindir="$BINDIR" \
			--inputdir="$SN" \
			--expecteddir="$SN" \
			--outputdir="$R" \
			--dlpath="$PGSUITE" \
			--schedule="$SN/schedule" \
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
	for t in test_setup gp_setup "${run_tests[@]}"; do
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
