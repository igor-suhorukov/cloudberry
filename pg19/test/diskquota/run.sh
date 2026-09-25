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
# Cloudberry's diskquota tests, on a cluster: M5's.
#
# gpcontrib/diskquota/tests/regress is what Cloudberry runs against its demo
# cluster -- a coordinator and three segments -- for diskquota, with its
# library preloaded, in the database contrib_regression, which has the
# extensions gp_inject_fault and diskquota_test.  The port runs it the same
# way, on a coordinator and three segments of its own, in the schedule's
# order and groups: manifest says of each test of the schedule whether it
# runs and, if not, why.  The launcher's database, diskquota, is made before
# the cluster's first start, as Cloudberry's tests expect it made; and what
# Cloudberry has built in, the port has as extensions, gp_core's, gp_sql's
# and gp_ao's, in every database the tests use.
#
# The tests set their settings with gpconfig and restart the cluster with
# gpstop, which here are the isolation2 suite's (../isolation2/bin), with one
# difference: the preloaded libraries a test sets -- diskquota's alone, or
# none -- are the port's modules with diskquota's, or without it, since a
# node of the port does not start without gp_core.
#
# Each test is compared as Cloudberry's pg_regress compares it: gpdiff.pl
# under Cloudberry's init files, diskquota's and the port's, or exactly a
# difference in cloudberry/, reviewed and kept (see ../singlenode/canon.pl
# for the form).  One pass: diskquota's worker plans without ORCA, and its
# tests ask the cluster about sizes, not about plans.  The two schedules are
# the suite's two parts, regress and isolation2, which DQ_PARTS names, each
# on a cluster of its own when they run as two jobs (../jobs).

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DQ="${CB_DISKQUOTA_DIR:-/cb/gpcontrib/diskquota/tests}"
CB="${CB_REGRESS_DIR:-/cb/src/test/regress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"

if [ ! -f "$DQ/regress/diskquota_schedule" ] || [ ! -x "$PG_REGRESS" ] ||
   [ ! -f "$CB/gpdiff.pl" ] ||
   [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/diskquota_test.control" ]; then
	echo "diskquota's tests, pg_regress, gpdiff.pl or diskquota_test is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-diskquota-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbd-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
# Nor is "diskquota" in its name: test_postmaster_restart looks for the
# worker with pgrep -f "[p]ostgres.*diskquota.*isolation2test", which the
# isolation2 driver's own command line matches where $HOME is
# /home/postgres.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-dq-XXXXXX")"
BASEPORT="${PGPORT:-$((7100 + RANDOM % 200))}"
NODES=4					# a coordinator and Cloudberry's three segments
MODULES='gp_core,gp_orca,gp_sql,gp_ao'
PRELOAD="$MODULES,diskquota-2.3"
SECRET="diskquota-schedule-$RANDOM$RANDOM$RANDOM"

port() { echo $((BASEPORT + $1)); }
# The superuser is Cloudberry's demo cluster's, gpadmin: its expected output
# names it.
export PGPORT="$(port 0)" PGHOST="$SOCK/n0" PGUSER=gpadmin

cleanup() {
	for n in $(seq 0 $((NODES - 1))); do
		[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/node$n.log" "$RESULTS_DIR/diskquota-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$WORK/node$n" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

run_tests=$(awk '$1 == "run" { print $2 }' "$HERE/manifest")

echo "diskquota: Cloudberry's diskquota tests, on a coordinator and three segments"
printf '  of the %d tests of the schedule the manifest lists: %d run here, %d are skipped\n' \
	"$(awk '$1 == "run" || $1 == "skip"' "$HERE/manifest" | wc -l)" \
	"$(echo "$run_tests" | wc -w)" \
	"$(awk '$1 == "skip"' "$HERE/manifest" | wc -l)"
echo

# The cluster, as the greenplum suite makes one; each node's log beside its
# data directory, where gpstop starts it with one.
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
		echo "max_worker_processes = 20"
		# Cloudberry's autovacuum passes over user tables, and the sizes the
		# tests show are theirs before any VACUUM; the port's would add a
		# visibility map when it came to them.
		echo "autovacuum = off"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$WORK/node$n/postgresql.auto.conf"
done
start_nodes() {
	for n in $(seq 1 $((NODES - 1))) 0; do
		"$BINDIR/pg_ctl" -D "$WORK/node$n" -l "$WORK/node$n.log" -w -t 60 start > /dev/null 2>&1 \
			|| { echo "node $n did not start"; tail -20 "$WORK/node$n.log"; exit 1; }
	done
}
start_nodes
# The port's extensions where gpconfig and gpstop ask which nodes there are,
# and in template1, for the databases the tests make; the launcher's
# database, and a start that finds it.
for db in postgres template1; do
	"$PSQL" -X -q -d "$db" -c "CREATE EXTENSION gp_core" -c "CREATE EXTENSION gp_sql" \
		-c "CREATE EXTENSION gp_ao" ||
		{ echo "could not create the port's extensions in $db"; exit 1; }
done
"$PSQL" -X -q -d postgres -c "CREATE DATABASE diskquota" ||
	{ echo "could not create the database diskquota"; exit 1; }
for n in 0 $(seq 1 $((NODES - 1))); do
	"$BINDIR/pg_ctl" -D "$WORK/node$n" -m fast -w -t 60 stop > /dev/null 2>&1
done
start_nodes

# gpconfig and gpstop, the isolation2 suite's; the preloaded libraries a test
# sets, the port's modules with diskquota's or without it.
mkdir -p "$EXEC/bin"
cp "$HERE/../isolation2/bin/gpstop" "$EXEC/bin/gpstop"
cat > "$EXEC/bin/gpconfig" <<GPCONFIG
#!/bin/bash
args=("\$@")
for i in "\${!args[@]}"; do
	if [ "\${args[\$i]}" = shared_preload_libraries ] && [ "\${args[\$((i + 1))]:-}" = -v ]; then
		case "\${args[\$((i + 2))]}" in
			*diskquota*) args[\$((i + 2))]='$PRELOAD' ;;
			*) args[\$((i + 2))]='$MODULES' ;;
		esac
	fi
done
exec bash "$HERE/../isolation2/bin/gpconfig" "\${args[@]}"
GPCONFIG
chmod +x "$EXEC/bin/gpstop" "$EXEC/bin/gpconfig"

# The suite, from the directory Cloudberry's tests run from: data/ has the
# name of diskquota's library, as the version file gives it there.
SN="$WORK/dq"
mkdir -p "$SN/sql" "$SN/expected" "$SN/data"
printf '#!/bin/bash\necho -n diskquota-2.3.so\n' > "$EXEC/current_binary_name"
chmod +x "$EXEC/current_binary_name"
ln -s "$EXEC/current_binary_name" "$SN/data/current_binary_name"
cp "$DQ"/regress/sql/*.sql "$SN/sql/"
cp "$DQ"/regress/expected/*.out "$SN/expected/"
# Cloudberry's schedule, its groups kept, less the tests the manifest skips
awk -v tests=" $(echo $run_tests) " '
	/^test:/ {
		line = ""
		for (i = 2; i <= NF; i++)
			if (index(tests, " " $i " ")) line = line " " $i
		if (line != "") print "test:" line
	}' "$DQ/regress/diskquota_schedule" > "$SN/schedule"

mkdir -p "$WORK/gpdiff"
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, with a difference reviewed and kept
# counted as none, as the greenplum suite has it.
cat > "$EXEC/bin/diff" <<EOF2
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
    --gpd_init "$CB/init_file" --gpd_init "$DQ/init_file" --gpd_init "$HERE/init_file")
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
EOF2
chmod +x "$EXEC/bin/diff"
mkdir -p "$WORK/canon"

PARTS="${DQ_PARTS:-regress isolation2}"
rc=0
if [[ " $PARTS " == *" regress "* ]]; then
echo "== regress: $DQ/regress"
cd "$SN"
PATH="$EXEC/bin:$PATH" COORDINATOR_DATA_DIRECTORY="$WORK/node0" PG_BINDIR="$BINDIR" \
	"$PG_REGRESS" \
		--bindir="$BINDIR" \
		--inputdir="$SN" \
		--expecteddir="$SN" \
		--outputdir="$WORK/out" \
		--schedule="$SN/schedule" \
		--load-extension=gp_core \
		--load-extension=gp_sql \
		--load-extension=gp_ao \
		--load-extension=gp_inject_fault \
		--load-extension=diskquota_test \
		--dbname=contrib_regression \
		--host="$SOCK/n0" --port="$(port 0)" \
	> "$WORK/pg_regress.out" 2>&1
rc=$?

grep -E "^(not )?ok " "$WORK/pg_regress.out" | sed 's/^/  /'
total=$(grep -cE "^(not )?ok " "$WORK/pg_regress.out")
bad=$(grep -cE "^not ok " "$WORK/pg_regress.out")
echo "  $((total - bad)) of $total passed"
sed -nE 's/^ok +[0-9]+ +[-+] +([^ ]+) .*/\1/p' "$WORK/pg_regress.out" |
while read -r t; do
	rm -f "$WORK/canon/$t.diff"
done
fi

# The isolation2 tests, on the same cluster, in the database the schedule's
# tests make and use, isolation2test: Cloudberry's driver, run as the
# isolation2 suite runs it (../isolation2/run.sh) -- less "-c gp_role=utility"
# for a session of a segment's own, from Cloudberry's isolation2 directory,
# where it finds its shell helpers.  A test's @VAR@ are what diskquota's
# CMakeLists.txt sets them to, as the port's cluster spells them: PL/Python 3,
# and a coordinator started as gpstop starts it.
[[ " $PARTS " == *" isolation2 "* ]] && echo "== isolation2: $DQ/isolation2"
iso_tests=$(awk '$1 == "run" && $2 ~ /^isolation2\// { sub(/^isolation2\//, "", $2); print $2 }' "$HERE/manifest")
iso_rc=0; iso_total=0; iso_bad=0
if [[ " $PARTS " == *" isolation2 "* ]] && [ -n "$iso_tests" ]; then
	CBI="${CB_ISOLATION2_DIR:-/cb/src/test/isolation2}"
	sed 's/given_opt="-c gp_role=utility"/given_opt=None/' "$CBI/sql_isolation_testcase.py" \
		> "$EXEC/sql_isolation_testcase.py"
	R="$WORK/iso"
	mkdir -p "$R/sql" "$R/expected" "$R/results" "$R/canon"
	# made from template1, which has the port's extensions
	"$PSQL" -X -q -d postgres -c "CREATE DATABASE isolation2test" > /dev/null &&
	"$PSQL" -X -q -d isolation2test -c "CREATE EXTENSION gp_inject_fault" > /dev/null ||
		{ echo "  the database isolation2test could not be made"; iso_rc=1; }
	# the expected output has the command as Cloudberry's CMakeLists.txt made it
	iso_convert() {
		sed -e 's/@PLPYTHON_LANG_STR@/plpython3u/g' \
			-e "s#@POSTMASTER_START_CMD@#pg_ctl -D \$COORDINATOR_DATA_DIRECTORY -l \$COORDINATOR_DATA_DIRECTORY.log -w start#g" \
			-e 's#pg_ctl -D \$COORDINATOR_DATA_DIRECTORY -w -o "-c gp_role=dispatch" start#pg_ctl -D $COORDINATOR_DATA_DIRECTORY -l $COORDINATOR_DATA_DIRECTORY.log -w start#g' \
			-e 's#\$(\./data/current_binary_name)#diskquota-2.3.so#g' \
			-e 's/-c gp_role=utility//g' "$1"
	}
	for t in $(awk '/^test:/ { for (i = 2; i <= NF; i++) print $i }' "$DQ/isolation2/isolation2_schedule"); do
		[[ " $(echo $iso_tests) " == *" $t "* ]] || continue
		[ "$iso_rc" -eq 0 ] || break
		src="$DQ/isolation2/sql/$t.sql"
		[ -f "$src" ] || src="$DQ/isolation2/sql/$t.in.sql"
		iso_convert "$src" > "$R/sql/$t.sql"
		iso_convert "$DQ/isolation2/expected/$t.out" > "$R/expected/$t.out"
		res="$R/results/$t.out"
		( cd "$CBI" && PATH="$EXEC/bin:$PATH" COORDINATOR_DATA_DIRECTORY="$WORK/node0" \
			PG_BINDIR="$BINDIR" timeout 600 python3 "$EXEC/sql_isolation_testcase.py" \
				--dbname=isolation2test --initfile_prefix="$res" \
				< "$R/sql/$t.sql" > "$res" 2>&1 )
		inits=(--gpd_init "$HERE/init_file" --gpd_init "$CB/init_file"
		       --gpd_init "$CBI/init_file_isolation2" --gpd_init "$DQ/init_file")
		[ -s "$res.initfile" ] && inits+=(--gpd_init "$res.initfile")
		env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U0 -I HINT: -I CONTEXT: -I GP_IGNORE: \
			"${inits[@]}" "$R/expected/$t.out" "$res" 2> /dev/null |
			perl "$HERE/../singlenode/canon.pl" > "$R/canon/$t.diff"
		st=("${PIPESTATUS[@]}")
		[ "${st[0]}" -le 1 ] && [ "${st[1]}" -eq 0 ] || echo "no comparison was made" >> "$R/canon/$t.diff"
		iso_total=$((iso_total + 1))
		if [ ! -s "$R/canon/$t.diff" ] || cmp -s "$R/canon/$t.diff" "$HERE/cloudberry/isolation2/$t.diff"; then
			rm -f "$R/canon/$t.diff"
			echo "  ok     $t"
		else
			echo "  not ok $t"
			iso_bad=$((iso_bad + 1))
			env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U3 -I HINT: -I CONTEXT: -I GP_IGNORE: \
				"${inits[@]}" "$R/expected/$t.out" "$res" >> "$R/regression.diffs" 2> /dev/null
		fi
	done
	echo "  $((iso_total - iso_bad)) of $iso_total passed"
	[ "$iso_bad" -eq 0 ] || iso_rc=1
fi

unreviewed=$( (cd "$WORK/canon" && ls -- *.diff 2> /dev/null;
			   cd "$WORK/iso/canon" 2> /dev/null && ls -- *.diff 2> /dev/null | sed 's#^#isolation2/#') |
			 sed 's/\.diff$//')
if [ -n "$unreviewed" ]; then
	echo "  differences no one has reviewed, from the expected output named:"
	echo $unreviewed | fold -s -w 72 | sed 's/^/    /'
fi
if { [ "$rc" -ne 0 ] || [ "$iso_rc" -ne 0 ]; } && [ -n "${RESULTS_DIR:-}" ]; then
	cp "$WORK/out/regression.diffs" "$RESULTS_DIR/diskquota.diffs" 2> /dev/null
	mkdir -p "$RESULTS_DIR/diskquota-canon" "$RESULTS_DIR/diskquota-results"
	cp "$WORK"/canon/*.diff "$RESULTS_DIR/diskquota-canon/" 2> /dev/null
	cp -r "$WORK"/out/results/. "$RESULTS_DIR/diskquota-results/" 2> /dev/null
	cp "$WORK/pg_regress.out" "$RESULTS_DIR/diskquota-pg_regress.out" 2> /dev/null
	if [ -d "$WORK/iso" ]; then
		mkdir -p "$RESULTS_DIR/diskquota-isolation2-canon" "$RESULTS_DIR/diskquota-isolation2-results"
		cp "$WORK"/iso/canon/*.diff "$RESULTS_DIR/diskquota-isolation2-canon/" 2> /dev/null
		cp -r "$WORK"/iso/results/. "$RESULTS_DIR/diskquota-isolation2-results/" 2> /dev/null
		cp "$WORK/iso/regression.diffs" "$RESULTS_DIR/diskquota-isolation2.diffs" 2> /dev/null
	fi
fi

[ "$rc" -eq 0 ] && [ "$iso_rc" -eq 0 ]
