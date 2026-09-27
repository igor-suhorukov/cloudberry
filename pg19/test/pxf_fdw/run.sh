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
# M8: Cloudberry's pxf_fdw, the foreign-data wrapper of a PXF server.
#
#   1. Cloudberry's four tests (gpcontrib/pxf_fdw/sql: the wrapper, a server,
#      a user mapping and a foreign table), which make and alter the objects
#      and ask the wrapper's validator about their options -- DDL a cluster
#      answers without a PXF server -- as Cloudberry's pg_regress runs them,
#      on one node and on a coordinator with two segments, each compared as
#      the diskquota suite compares Cloudberry's: gpdiff.pl under Cloudberry's
#      init file, or exactly a difference kept in cloudberry/ (README);
#   2. a scan and an INSERT through a stand-in for PXF (pxf_standin.py, a
#      server of the suite's own answering the Fragmenter, the Bridge and the
#      Writable endpoints pxf_fdw asks), on one node and on the cluster: the
#      rows of every fragment, a rescan, single row error handling, and rows
#      sent -- on the cluster by the segments, the wrapper's mpp_execute being
#      'all segments', each reading its share of the fragments;
#   3. mpp_execute and num_segments, which gp_core keeps as Cloudberry does
#      (gp_foreign.c), on the cluster: taken by a wrapper whose validator
#      refuses an option it does not know, postgres_fdw's, set on every node,
#      checked in Cloudberry's words, and a table read on every segment or on
#      the coordinator alone as they say, under both planners.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/pxf_fdw/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PXF="${CB_PXF_DIR:-/cb/gpcontrib/pxf_fdw}"
CB="${CB_REGRESS_DIR:-/cb/src/test/regress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"

if [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/pxf_fdw.control" ] ||
   [ ! -d "$PXF/sql" ] || [ ! -x "$PG_REGRESS" ] || [ ! -f "$CB/gpdiff.pl" ]; then
	echo "pxf_fdw, its tests, pg_regress or gpdiff.pl is not installed; skipping"
	exit 77
fi

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-pxf-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbpxf-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-pxf-XXXXXX")"
BASEPORT="${PGPORT:-$((7300 + RANDOM % 200))}"
PXF_PORT=$((BASEPORT + 20))
PRELOAD='gp_core,gp_orca,gp_sql'
SECRET="pxf-fdw-$RANDOM$RANDOM$RANDOM"
TESTS="pxf_fdw_wrapper pxf_fdw_server pxf_fdw_user_mapping pxf_fdw_foreign_table"

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | head -12 | sed 's/^/         /'
         fail=$((fail + 1)); }

# node 0 is the coordinator, 1 and 2 the segments; node 9 is the one node
datadir() { echo "$ROOT/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }
NODES="0 1 2"

cleanup() {
	for n in $NODES 9; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/pxf-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${STANDIN_PID:-}" ] && kill "$STANDIN_PID" 2> /dev/null
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

q() {						# q <node> <db> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d "$2" -c "$3" 2>&1
}
is() {						# is <name> <node> <db> <sql> <want>
	local got; got=$(q "$2" "$3" "$4")
	[ "$got" = "$5" ] && ok "$1" || notok "$1" "want [$5], got [$got]"
}

echo "M8 pxf_fdw"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

CONF="$ROOT/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
done > "$CONF"
for n in $NODES 9; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 > "$ROOT/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "fsync = off"
		if [ "$n" != 9 ]; then
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			echo "max_prepared_transactions = 16"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		fi
	} >> "$(datadir "$n")/postgresql.auto.conf"
done
for n in 1 2 0 9; do
	"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "node $n did not start"; tail -20 "$ROOT/node$n.log"; exit 1; }
done
# The port's extensions in template1, for the databases the tests make.
for n in 0 9; do
	q "$n" template1 "CREATE EXTENSION gp_core; CREATE EXTENSION gp_sql" > /dev/null ||
		{ echo "could not create the port's extensions on node $n"; exit 1; }
done

mkdir -p "$ROOT/gpdiff" "$EXEC/bin"
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$ROOT/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$ROOT/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, with a difference reviewed and kept
# counted as none (see the diskquota suite).  TARGET names the kept
# difference of one target alone: <test>.<target>.diff before <test>.diff.
cat > "$EXEC/bin/diff" <<EOF2
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
    --gpd_init "$CB/init_file" --gpd_init "$HERE/init_file")
canon="$ROOT/canon-\$TARGET/\$name.diff"
env PATH=/usr/bin:/bin perl "$ROOT/gpdiff/gpdiff.pl" -U0 "\${cb[@]}" "\$exp" "\$res" 2> /dev/null |
	perl "$HERE/../singlenode/canon.pl" > "\$canon"
st=("\${PIPESTATUS[@]}")
[ "\${st[0]}" -le 1 ] && [ "\${st[1]}" -eq 0 ] || echo "no comparison was made" >> "\$canon"
kept="$HERE/cloudberry/\$name.\$TARGET.diff"
[ -f "\$kept" ] || kept="$HERE/cloudberry/\$name.diff"
if [ ! -s "\$canon" ] || cmp -s "\$canon" "\$kept"; then
	rm -f "\$canon"
	exit 0
fi
exec env PATH=/usr/bin:/bin perl "$ROOT/gpdiff/gpdiff.pl" "\${opts[@]}" "\${cb[@]}" "\$exp" "\$res"
EOF2
chmod +x "$EXEC/bin/diff"

rc=0
# regress <target> <node>: Cloudberry's four tests, in its Makefile's order
regress() {
	local target="$1" node="$2" out="$ROOT/out-$1"
	mkdir -p "$ROOT/canon-$target" "$out"
	( cd "$PXF" && TARGET="$target" PATH="$EXEC/bin:$PATH" "$PG_REGRESS" \
		--bindir="$BINDIR" \
		--inputdir="$PXF" \
		--expecteddir="$PXF" \
		--outputdir="$out" \
		--dbname=contrib_regression \
		--host="$(sockdir "$node")" --port="$(port "$node")" \
		$TESTS ) > "$out/pg_regress.out" 2>&1
	local total bad
	total=$(grep -cE "^(not )?ok " "$out/pg_regress.out")
	bad=$(grep -cE "^not ok " "$out/pg_regress.out")
	grep -E "^(not )?ok " "$out/pg_regress.out" | sed 's/^/  /'
	if [ "$total" -eq 4 ] && [ "$bad" -eq 0 ]; then
		ok "Cloudberry's four tests, $target"
	else
		notok "Cloudberry's four tests, $target" "$(tail -5 "$out/pg_regress.out")"
		[ -n "${RESULTS_DIR:-}" ] && cp "$out/regression.diffs" "$RESULTS_DIR/pxf-$target.diffs" 2> /dev/null
		ls "$ROOT/canon-$target"
	fi
}

###############################################################################
echo "1. Cloudberry's tests"
###############################################################################
regress one_node 9
regress cluster 0

###############################################################################
echo "2. a scan and an INSERT through a stand-in for PXF"
###############################################################################
# three fragments of a table's, 30 rows, and one with a row that will not parse
DATA="$ROOT/pxf"
mkdir -p "$DATA/rows" "$DATA/bad"
for f in 1 2 3; do
	for i in $(seq $(((f - 1) * 10 + 1)) $((f * 10))); do echo "$i,name $i"; done > "$DATA/rows/part$f"
done
printf '1,one\ntwo,2\n3,three\n' > "$DATA/bad/part1"
python3 "$HERE/pxf_standin.py" "$PXF_PORT" "$DATA" > "$ROOT/standin.log" 2>&1 &
STANDIN_PID=$!
for i in $(seq 1 50); do
	curl -s -o /dev/null "http://127.0.0.1:$PXF_PORT/" && break
	sleep 0.1
done

# standin <target> <node>: the checks, on one node or on the cluster
standin() {
	local target="$1" node="$2" out
	q "$node" postgres "CREATE DATABASE standin" > /dev/null
	out=$(q "$node" standin "
		CREATE EXTENSION pxf_fdw;
		CREATE EXTENSION gp_exttable;
		CREATE SERVER standin FOREIGN DATA WRAPPER file_pxf_fdw
			OPTIONS (pxf_host '127.0.0.1', pxf_port '$PXF_PORT');
		CREATE USER MAPPING FOR CURRENT_USER SERVER standin;
		CREATE FOREIGN TABLE rows (id int, name text) SERVER standin
			OPTIONS (resource '/rows');
		CREATE FOREIGN TABLE bad (id int, name text) SERVER standin
			OPTIONS (resource '/bad', reject_limit '5', log_errors 'true');
		CREATE FOREIGN TABLE bad_strict (id int, name text) SERVER standin
			OPTIONS (resource '/bad');
		CREATE FOREIGN TABLE sink (id int, name text) SERVER standin
			OPTIONS (resource '/sink-$target');")
	[ -z "$out" ] && ok "$target: the wrapper, a server, a user mapping and tables made" \
		|| notok "$target: the objects" "$out"
	: > "$DATA/requests.log"
	is "$target: a scan reads every fragment's rows" "$node" standin \
	   "SELECT count(*), sum(id), max(name) FROM rows" "30|465|name 9"
	cp "$DATA/requests.log" "$DATA/first-$target.log"
	out=$(grep -c '^bridge ' "$DATA/first-$target.log")
	[ "$out" = 3 ] && ok "$target: each of the three fragments asked of PXF's Bridge once" \
		|| notok "$target: the Bridge's requests" "$(cat "$DATA/first-$target.log")"
	is "$target: a filter pushed down to PXF, and checked again" "$node" standin \
	   "SELECT count(*) FROM rows WHERE id > 25" "5"
	grep -q '^bridge .*filter=a0c23s2d25o2' "$DATA/requests.log" && ok "$target: the filter sent as X-GP-FILTER" \
		|| notok "$target: X-GP-FILTER" "$(tail -3 "$DATA/requests.log")"
	is "$target: a rescan reads the fragments again, for each outer row" "$node" standin \
	   "SELECT string_agg((SELECT count(*) FROM rows WHERE id > g)::text, ',' ORDER BY g)
	      FROM generate_series(0, 20, 10) g" "30,20,10"
	out=$(q "$node" standin "SELECT count(*) FROM bad")
	case "$out" in
		*"found 1 data formatting errors (1 or more input rows), rejected related input data"*"2")
			ok "$target: a row that will not parse rejected under reject_limit, and said" ;;
		*) notok "$target: reject_limit" "$out" ;;
	esac
	is "$target: and logged, with its line and its error, as log_errors says" "$node" standin \
	   "SELECT linenum, rawdata, errmsg FROM gp_read_error_log('bad')" \
	   '2|two,2|invalid input syntax for type integer: "two", column id'
	out=$(q "$node" standin "SELECT count(*) FROM bad_strict")
	case "$out" in
		*'invalid input syntax for type integer: "two"'*) ok "$target: without reject_limit, the scan fails at it" ;;
		*) notok "$target: a table without reject_limit" "$out" ;;
	esac
	if [ "$target" = cluster ]; then
		out=$(grep '^fragments ' "$DATA/first-$target.log" | sort | tr '\n' ' ')
		[ "$out" = "fragments segment=0 count=3 filter= fragments segment=1 count=3 filter= " ] &&
			ok "$target: each segment asks PXF for the fragments, the wrapper's mpp_execute being 'all segments'" ||
			notok "$target: the segments' Fragmenter requests" "$out"
		out=$(grep '^bridge ' "$DATA/first-$target.log" | sed 's/ fragment=.*//' | sort -u | tr '\n' ' ')
		[ "$out" = "bridge segment=0 bridge segment=1 " ] &&
			ok "$target: and reads its share of them, the three shared by the two" ||
			notok "$target: the segments' shares" "$(grep '^bridge ' "$DATA/first-$target.log")"
	fi
	out=$(q "$node" standin "INSERT INTO sink SELECT g, 'row ' || g FROM generate_series(1, 4) g")
	written=$(cat "$DATA/sink-$target"/written-* 2> /dev/null | sort -t, -k1n | tr '\n' ' ')
	[ -z "$out" ] && [ "$written" = "1,row 1 2,row 2 3,row 3 4,row 4 " ] &&
		ok "$target: an INSERT's rows sent to PXF's Writable endpoint as CSV" ||
		notok "$target: the rows written" "$out / $written"
}
standin one_node 9
standin cluster 0

###############################################################################
echo "3. mpp_execute and num_segments, on the cluster"
###############################################################################
out=$(q 0 standin "
	CREATE FOREIGN DATA WRAPPER dummy OPTIONS (mpp_execute 'coordinator');
	CREATE SERVER dummy_s FOREIGN DATA WRAPPER dummy OPTIONS (num_segments '1');
	CREATE FOREIGN TABLE dummy_t (a int) SERVER dummy_s
		OPTIONS (delimiter ',', mpp_execute 'all segments', num_segments '2');")
[ -z "$out" ] && ok "a wrapper, a server and a table that set them" || notok "the objects that set them" "$out"
is "kept among the table's options, as Cloudberry keeps them, on every node" 0 standin \
   "SELECT string_agg(result, ' ' ORDER BY content) FROM gp.exec_on_segments(
      \$\$SELECT ftoptions::text FROM pg_foreign_table WHERE ftrelid = 'dummy_t'::regclass\$\$)" \
   '{"delimiter=,","mpp_execute=all segments",num_segments=2} {"delimiter=,","mpp_execute=all segments",num_segments=2}'
out=$(q 0 standin "ALTER FOREIGN DATA WRAPPER dummy OPTIONS (SET mpp_execute 'all segments')")
[ "$out" = 'ERROR:  "mpp_execute" of foreign data wrapper is not allowed to be altered' ] &&
	ok "a wrapper's mpp_execute is not altered, in Cloudberry's words" || notok "ALTER of a wrapper's mpp_execute" "$out"
out=$(q 0 standin "ALTER FOREIGN TABLE dummy_t OPTIONS (SET mpp_execute 'a')")
[ "$out" = 'ERROR:  "a" is not a valid mpp_execute value' ] &&
	ok "nor set to what it cannot be" || notok "an mpp_execute of 'a'" "$out"
out=$(q 0 standin "ALTER SERVER dummy_s OPTIONS (SET num_segments '0')")
[ "$out" = 'ERROR:  "0" is not a valid num_segments value' ] &&
	ok "nor num_segments" || notok "a num_segments of 0" "$out"
if [ -f "$("$BINDIR/pg_config" --sharedir)/extension/postgres_fdw.control" ]; then
	out=$(q 0 standin "
		CREATE EXTENSION postgres_fdw;
		CREATE TABLE remote (a int, b text);
		INSERT INTO remote SELECT g, 'r' || g FROM generate_series(1, 10) g;
		CREATE SERVER loop FOREIGN DATA WRAPPER postgres_fdw
			OPTIONS (host '$(sockdir 0)', port '$(port 0)', dbname 'standin', mpp_execute 'all segments');
		CREATE USER MAPPING FOR CURRENT_USER SERVER loop;
		ALTER SERVER loop OPTIONS (ADD fetch_size '100');
		CREATE FOREIGN TABLE on_segments (a int, b text) SERVER loop OPTIONS (table_name 'remote');
		CREATE FOREIGN TABLE on_coordinator (a int, b text) SERVER loop
			OPTIONS (table_name 'remote', mpp_execute 'coordinator');" 2>&1 | grep -v '^NOTICE\|^HINT')
	[ -z "$out" ] && ok "postgres_fdw's validator, which refuses an option it does not know, takes them, and an ALTER after" \
		|| notok "postgres_fdw with mpp_execute" "$out"
	for opt in on off; do
		is "gp.optimizer=$opt: a table whose server says 'all segments' is read by each segment" 0 standin \
		   "SET gp.optimizer = $opt; SELECT string_agg(gp_segment_id || ':' || n, ' ' ORDER BY gp_segment_id)
		      FROM (SELECT gp_segment_id, count(*) n FROM on_segments GROUP BY 1) s" "0:10 1:10"
		is "gp.optimizer=$opt: its aggregate the segments', not the wrapper's server's for the coordinator" 0 standin \
		   "SET gp.optimizer = $opt; SELECT count(*), sum(a) FROM on_segments" "20|110"
		is "gp.optimizer=$opt: a table whose own says 'coordinator' is read there" 0 standin \
		   "SET gp.optimizer = $opt; SELECT count(*), sum(a) FROM on_coordinator" "10|55"
	done
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
