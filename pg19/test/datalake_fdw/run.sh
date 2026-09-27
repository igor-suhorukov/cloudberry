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
# Cloudberry's datalake_fdw tests, on one node and on a cluster: M8's.
#
# contrib/datalake_fdw's installcheck is what Cloudberry's CI runs against its
# demo cluster -- a coordinator and three segments -- with the library
# preloaded (ic-datalake-fdw, .github/workflows/build-cloudberry.yml): its
# three cases, iceberg_am_ddl, iceberg_am_reject and iceberg_am_acl, in
# test/automation/sqlrepo/smoke/iceberg_am, which are also the one smoke
# category of its automation, the one that needs no service (its runner,
# run_smoke_tests.sh, runs installcheck).  The port runs them on one node,
# which has no segments, and on a coordinator and three segments of its own,
# with the port's modules and datalake_fdw preloaded after them, in the
# database pg_regress makes, which has gp_core's and gp_sql's extensions.
#
# Each test is compared as Cloudberry's pg_regress compares it: gpdiff.pl,
# under Cloudberry's init_file and the port's greenplum one, or exactly a
# difference in cloudberry/, reviewed and kept -- <test>.diff on the cluster,
# <test>.single.diff on one node, and either with .planner or .orca before
# .diff for one pass alone (see ../singlenode/canon.pl for the form).  Two
# passes, the planner (gp.optimizer = off) and ORCA, as PASSES says.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DL="${CB_DATALAKE_DIR:-/cb/contrib/datalake_fdw}"
CASES="$DL/test/automation/sqlrepo/smoke/iceberg_am"
CB="${CB_REGRESS_DIR:-/cb/src/test/regress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"
TESTS="iceberg_am_ddl iceberg_am_reject iceberg_am_acl"

if [ ! -d "$CASES/sql" ] || [ ! -x "$PG_REGRESS" ] || [ ! -f "$CB/gpdiff.pl" ] ||
   [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/datalake_fdw.control" ]; then
	echo "datalake_fdw's tests, pg_regress, gpdiff.pl or datalake_fdw is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-datalake-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbdl-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-dl-XXXXXX")"
BASEPORT="${PGPORT:-$((7300 + RANDOM % 200))}"
SECRET="datalake-fdw-$RANDOM$RANDOM$RANDOM"
NODES="0 1 2 3"			# a coordinator and Cloudberry's three segments; 9 is the one node

port() { echo $((BASEPORT + $1)); }
# The superuser is Cloudberry's demo cluster's, gpadmin.
export PGUSER=gpadmin

cleanup() {
	for n in $NODES 9; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/node$n.log" "$RESULTS_DIR/datalake-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$WORK/node$n" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

echo "datalake_fdw: Cloudberry's datalake_fdw tests, on one node and on a coordinator and three segments"
echo

# The nodes: the cluster, as the greenplum suite makes one, and the one node,
# each with datalake_fdw preloaded after the port's modules, as its hooks are
# to see a statement before gp_sql and gp_core do.
CONF="$WORK/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $SOCK/n$n $(port "$n") $WORK/node$n"
done > "$CONF"
for n in $NODES 9; do
	mkdir -p "$SOCK/n$n"
	"$BINDIR/initdb" -D "$WORK/node$n" -N -U gpadmin --locale=C --encoding=UTF8 > "$WORK/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$WORK/initdb$n.log"; exit 1; }
	{
		echo "unix_socket_directories = '$SOCK/n$n'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "fsync = off"
		echo "shared_preload_libraries = 'gp_core,gp_orca,gp_sql,datalake_fdw'"
		if [ "$n" != 9 ]; then
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			echo "max_prepared_transactions = 64"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		fi
	} >> "$WORK/node$n/postgresql.auto.conf"
done
for n in 1 2 3 0 9; do
	"$BINDIR/pg_ctl" -D "$WORK/node$n" -l "$WORK/node$n.log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "node $n did not start"; tail -20 "$WORK/node$n.log"; exit 1; }
done

# The cases, from the automation's directory, as the module's Makefile points
# pg_regress at it.
SN="$WORK/cases"
mkdir -p "$SN/sql" "$SN/expected"
for t in $TESTS; do
	cp "$CASES/sql/$t.sql" "$SN/sql/"
	cp "$CASES/expected/$t.out" "$SN/expected/"
done

mkdir -p "$WORK/gpdiff"
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, with a difference reviewed and kept
# counted as none.  DL_WHERE is "" on the cluster and ".single" on the one
# node, DL_PASS the pass, which name a difference kept for them.
mkdir -p "$EXEC/bin"
cat > "$EXEC/bin/diff" <<EOF
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)\$DL_WHERE
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
    --gpd_init "$CB/init_file" --gpd_init "$HERE/../greenplum/init_file")
canon="$WORK/canon/\$DL_PASS/\$name.diff"
mkdir -p "$WORK/canon/\$DL_PASS"
env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U0 "\${cb[@]}" "\$exp" "\$res" 2> /dev/null |
	perl "$HERE/../singlenode/canon.pl" > "\$canon"
st=("\${PIPESTATUS[@]}")
[ "\${st[0]}" -le 1 ] && [ "\${st[1]}" -eq 0 ] || echo "no comparison was made" >> "\$canon"
if [ ! -s "\$canon" ] || cmp -s "\$canon" "$HERE/cloudberry/\$name.\$DL_PASS.diff" ||
   cmp -s "\$canon" "$HERE/cloudberry/\$name.diff"; then
	rm -f "\$canon"
	exit 0
fi
exec env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" "\${opts[@]}" "\${cb[@]}" "\$exp" "\$res"
EOF
chmod +x "$EXEC/bin/diff"

rc=0
for pass in ${PASSES:-planner orca}; do
	[ "$pass" = orca ] && optimizer=on || optimizer=off
	for where in single cluster; do
		[ "$where" = single ] && node=9 || node=0
		out="$WORK/out-$pass-$where"
		echo "== $pass, $([ "$where" = single ] && echo "one node" || echo "a coordinator and three segments")"
		DL_WHERE=$([ "$where" = single ] && echo .single) DL_PASS=$pass \
		PATH="$EXEC/bin:$PATH" \
		PGOPTIONS="-c gp.optimizer=$optimizer -c statement_timeout=${STATEMENT_TIMEOUT:-60s}" \
			"$PG_REGRESS" \
				--bindir="$BINDIR" \
				--inputdir="$SN" \
				--expecteddir="$SN" \
				--outputdir="$out" \
				--load-extension=gp_core \
				--load-extension=gp_sql \
				--dbname=contrib_regression \
				--host="$SOCK/n$node" --port="$(port "$node")" \
				$TESTS \
			> "$out.log" 2>&1
		[ $? -eq 0 ] || rc=1
		grep -E "^(not )?ok " "$out.log" | sed 's/^/  /'
		total=$(grep -cE "^(not )?ok " "$out.log")
		bad=$(grep -cE "^not ok " "$out.log")
		echo "  $((total - bad)) of $total passed"
		if [ "$total" -eq 0 ]; then
			tail -5 "$out.log" | sed 's/^/  /'
			rc=1
		fi
		if [ -n "${RESULTS_DIR:-}" ] && [ "$bad" -ne 0 ]; then
			mkdir -p "$RESULTS_DIR/datalake-$pass-$where"
			cp "$out/regression.diffs" "$RESULTS_DIR/datalake-$pass-$where.diffs" 2> /dev/null
			cp -r "$out"/results/. "$RESULTS_DIR/datalake-$pass-$where/" 2> /dev/null
		fi
	done
done

# What the port adds: a session of a segment's own, which Cloudberry calls
# utility mode, makes no lake table there, which would be that node's alone;
# the one node, which is its own coordinator, made the tests' own.
echo "== utility mode"
out=$("$PSQL" -X -q -h "$SOCK/n1" -p "$(port 1)" -d contrib_regression \
	-c "CREATE TABLE dlskel_utility (a int) USING iceberg WITH (catalog = 'c', volume = 'v')" 2>&1)
case "$out" in
	*"iceberg: utility-mode DDL on iceberg tables is not supported yet"*)
		echo "  ok     a segment's own session is refused a lake table" ;;
	*)
		echo "  NOT OK a segment's own session is refused a lake table"
		echo "$out" | head -5 | sed 's/^/         /'
		rc=1 ;;
esac

unreviewed=$(cd "$WORK/canon" 2> /dev/null && ls -- */*.diff 2> /dev/null | sed 's/\.diff$//')
if [ -n "$unreviewed" ]; then
	echo "  differences no one has reviewed, pass/test:"
	echo $unreviewed | fold -s -w 72 | sed 's/^/    /'
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/datalake-canon"
		cp -r "$WORK"/canon/. "$RESULTS_DIR/datalake-canon/"
	fi
fi

[ "$rc" -eq 0 ]
