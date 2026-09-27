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
# M8: Cloudberry's gpcloud, the s3:// protocol of external tables, against an
# S3 of the suite's own.
#
#   1. gpcloud's unit tests (gpcontrib/gpcloud/test), Cloudberry's googletest
#      program, gpcloud_test, where it is built -- but for the ones that ask
#      www.bing.com, a server on the internet, which a test run is not to
#      depend on;
#   2. the S3: moto's server (moto_server), which takes any region, as the
#      tests read thirteen regions'; the buckets and the data Cloudberry's
#      tests read, made by s3data.py.  gpcloud reaches it as an HTTP proxy --
#      each section of the configuration the suite writes, as
#      regress/generate_config_file.sh writes Cloudberry's, has "proxy" and
#      "encryption = false" -- so that the tests' locations, Amazon's
#      endpoints, stay as they are, Host and signature included.  Its IAM
#      authentication, which would check each request's signature, is left
#      off: moto 5.1.1 checks a query's canonical form from the URL decoded,
#      and refuses the signature of every listing whose prefix has a "/" --
#      gpcloud's, which encodes it as S3 asks, and curl's own alike.  The
#      signature gpcloud computes is its unit tests' (s3utils_test.cpp);
#   3. gpcheckcloud, the tool: a configuration checked, a file uploaded and
#      downloaded again, as regress/gpcheckcloud_regress.sh does;
#   4. Cloudberry's regression schedule (regress/regress_schedule), its
#      .source files converted as its Makefile converts them, the tests the
#      manifest runs, on a coordinator and three segments, as Cloudberry's
#      demo cluster runs them, and on one node; each compared as the
#      greenplum suite compares Cloudberry's -- gpdiff.pl under Cloudberry's
#      init file and the port's, or exactly a difference kept in cloudberry/;
#      and checks of the suite's own on each, of what the tests it skips
#      read Cloudberry's data sets for: a gzipped and a deflated object, one
#      of three chunks, and a location's objects shared among the segments.
#
# GPCLOUD_PARTS (unit s3 regress) and GPCLOUD_TARGETS (cluster one_node) name
# a part of it.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/gpcloud/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GPCLOUD="${CB_GPCLOUD_DIR:-/cb/gpcontrib/gpcloud}"
CB="${CB_REGRESS_DIR:-/cb/src/test/regress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"
PARTS="${GPCLOUD_PARTS:-unit s3 regress}"
TARGETS="${GPCLOUD_TARGETS:-cluster one_node}"

if [ ! -f "$("$BINDIR/pg_config" --pkglibdir)/gpcloud.so" ] || [ ! -x "$BINDIR/gpcheckcloud" ] ||
   [ ! -d "$GPCLOUD/regress" ] || [ ! -x "$PG_REGRESS" ] || [ ! -f "$CB/gpdiff.pl" ] ||
   ! command -v moto_server > /dev/null || ! python3 -c "import boto3" 2> /dev/null; then
	echo "gpcloud, its tests, pg_regress, gpdiff.pl or moto's S3 server is not installed; skipping"
	exit 77
fi

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-gpcloud-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbgc-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-gpcloud-XXXXXX")"
BASEPORT="${PGPORT:-$((7500 + RANDOM % 200))}"
S3PORT=$((BASEPORT + 20))
PRELOAD='gp_core,gp_orca,gp_sql'
SECRET="gpcloud-$RANDOM$RANDOM$RANDOM"
CONFIG="$ROOT/s3.conf"
# The superuser is Cloudberry's demo cluster's, gpadmin, as the greenplum
# suite's is.
export PGUSER=gpadmin

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | head -12 | sed 's/^/         /'
         fail=$((fail + 1)); }

# node 0 is the coordinator, 1..3 the segments; node 9 is the one node
datadir() { echo "$ROOT/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }
NODES="0 1 2 3"

cleanup() {
	for n in $NODES 9; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/gpcloud-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${S3_PID:-}" ] && kill "$S3_PID" 2> /dev/null
	pkill -f "[d]ummyHTTPServer.py -p 8553" 2> /dev/null
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

q() {						# q <node> <db> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d "$2" -c "$3" 2>&1
}

echo "M8 gpcloud"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

rc=0

###############################################################################
if [[ " $PARTS " == *" unit "* ]]; then
echo "1. gpcloud's unit tests"
###############################################################################
# The tests that ask www.bing.com over the internet, and expect its answers.
ONLINE='S3RESTfulService.GetWithWrongHeader:S3RESTfulService.GetWithEmptyHeader'
ONLINE+=':S3RESTfulService.GetWithWrongURL:S3RESTfulService.PutToServerWithBlindPutService'
ONLINE+=':S3RESTfulService.PutToServerWith404Page:S3RESTfulService.HeadWithCorrectURLAndDebugParam'
ONLINE+=':S3RESTfulService.HeadWithWrongURL:S3RESTfulService.HeadWithCorrectURL'
ONLINE+=':S3RESTfulService.PostToServerWithBlindPutServiceAndDebugParam'
ONLINE+=':S3RESTfulService.PostToServerWithBlindPutService:S3RESTfulService.PostToServerWith404Page'
ONLINE+=':S3RESTfulService.PostToServerWithData'
if [ -x "$BINDIR/gpcloud_test" ]; then
	# from the tests' directory, whose data/ they read
	( cd "$GPCLOUD/test" && "$BINDIR/gpcloud_test" --gtest_filter="-$ONLINE" ) > "$ROOT/gtest.out" 2>&1
	summary=$(grep -E '^\[  (PASSED|FAILED)  \]|tests? from .* test suites? ran' "$ROOT/gtest.out" | tr '\n' ' ')
	if grep -q '^\[  PASSED  \]' "$ROOT/gtest.out" && ! grep -q '^\[  FAILED  \]' "$ROOT/gtest.out"; then
		ok "gpcloud_test: $summary(12 that ask the internet left out)"
	else
		notok "gpcloud_test" "$(grep -E '^\[  FAILED  \]' "$ROOT/gtest.out")"
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/gtest.out" "$RESULTS_DIR/gpcloud-gtest.out"
	fi
else
	echo "  gpcloud_test is not built (no googletest in the tree): not run"
fi
fi

[[ " $PARTS " == *" s3 "* || " $PARTS " == *" regress "* ]] || { echo; echo "$pass passed, $fail failed"; [ "$fail" -eq 0 ]; exit; }

###############################################################################
echo "2. the S3"
###############################################################################
ACCESSID=gpcloudsuite
SECRETKEY="gpcloud-suite-$RANDOM$RANDOM"
( cd "$ROOT" && exec moto_server -H 127.0.0.1 -p "$S3PORT" ) > "$ROOT/moto.log" 2>&1 &
S3_PID=$!
for i in $(seq 1 100); do
	curl -s -o /dev/null "http://127.0.0.1:$S3PORT/moto-api/" && break
	sleep 0.1
done
if python3 "$HERE/s3data.py" "http://127.0.0.1:$S3PORT" "$ACCESSID" "$SECRETKEY" \
		> "$ROOT/s3data.log" 2>&1; then
	ok "moto's S3, its buckets and data"
else
	notok "moto's S3" "$(tail -5 "$ROOT/s3data.log")"
	echo; echo "$pass passed, $fail failed"; exit 1
fi

# The configuration, as generate_config_file.sh writes Cloudberry's -- its
# sections and their settings -- each reaching the S3 as its proxy, over
# HTTP; the section that names a proxy of its own, a SOCKS5 one in
# Cloudberry's, has the S3 as its proxy too, and the one that names a wrong
# one keeps it.
section() {					# section <name> <settings...>
	echo "[$1]"
	echo "accessid = \"$ACCESSID\""
	echo "secret = \"$SECRETKEY\""
	echo "encryption = false"
	shift
	printf '%s\n' "$@"
	echo "threadnum = 3"
	echo "chunksize = 16777217"
	echo
}
{
	section default "proxy = http://127.0.0.1:$S3PORT"
	section no_autocompress "proxy = http://127.0.0.1:$S3PORT" "autocompress = false"
	section sse_s3 "proxy = http://127.0.0.1:$S3PORT" "server_side_encryption = sse-s3"
	section proxy "proxy = http://127.0.0.1:$S3PORT"
	section wrong_proxy "proxy = socks5://127.0.0.1:1090"
} > "$CONFIG"

###############################################################################
echo "3. gpcheckcloud"
###############################################################################
URL="s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/regress"
out=$("$BINDIR/gpcheckcloud" -c "$URL/small17/ config=$CONFIG" 2>&1)
case "$out" in
	*"Your configuration works well."*) ok "-c: the configuration checked, the bucket listed" ;;
	*) notok "gpcheckcloud -c" "$out" ;;
esac
head -c 6 /dev/urandom > "$ROOT/check.small"; echo >> "$ROOT/check.small"
head -c 20000000 /dev/urandom > "$ROOT/check.large"; echo >> "$ROOT/check.large"
for f in small large; do
	out=$("$BINDIR/gpcheckcloud" -u "$ROOT/check.$f" "$URL/s3write/gpcheckcloud/$f/ config=$CONFIG" 2>&1) ||
		notok "-u: the $f file uploaded" "$out"
	want=$(md5sum < "$ROOT/check.$f")
	got=$("$BINDIR/gpcheckcloud" -d "$URL/s3write/gpcheckcloud/$f/ config=$CONFIG" 2> /dev/null | md5sum)
	[ "$got" = "$want" ] && ok "-u and -d: the $f file uploaded and downloaded again, the same" \
		|| notok "gpcheckcloud -d of the $f file" "want $want, got $got"
done
# gpcheckcloud_regress.sh's expected failure: an endpoint of no region's,
# which gpcheckcloud may not crash at
"$BINDIR/gpcheckcloud" -d "https://s3-us-external-1.amazonaws.com/us-east-1.s3test.pivotal.io/ config=$CONFIG" \
	> /dev/null 2>&1
st=$?
[ "$st" -lt 128 ] && ok "-d of an endpoint of no region's, without a crash" \
	|| notok "gpcheckcloud -d of an endpoint of no region's" "exit $st"

[[ " $PARTS " == *" regress "* ]] || { echo; echo "$pass passed, $fail failed"; [ "$fail" -eq 0 ]; exit; }

###############################################################################
echo "4. Cloudberry's regression schedule"
###############################################################################
run_tests=$(awk '$1 == "run" { print $2 }' "$HERE/manifest")
printf '  of the %d tests of the schedule the manifest lists: %d run here, %d are skipped\n' \
	"$(awk '$1 == "run" || $1 == "skip"' "$HERE/manifest" | wc -l)" \
	"$(echo "$run_tests" | wc -w)" "$(awk '$1 == "skip"' "$HERE/manifest" | wc -l)"

# The suite, as regress/Makefile makes it: the .source files with the
# configuration and the prefixes -- a directory of the run's and the
# target's for what the tests write -- and dummyHTTPServer.py beside them,
# where a test runs it from; with Python 3 as "python", which that test
# calls it with.
mkdir -p "$EXEC/bin"
ln -s "$(command -v python3)" "$EXEC/bin/python"
RUN="$(date +%Y%m%d)-$RANDOM$RANDOM"
source_replaced() {			# source_replaced <target>: the target's suite
	local src="$ROOT/source_replaced-$1" write="s3write/$RUN-$1" t
	mkdir -p "$src/sql" "$src/expected"
	cp "$GPCLOUD/bin/dummyHTTPServer.py" "$src/"
	for t in $run_tests; do
		for f in "input/$t.source:sql/$t.sql" "output/$t.source:expected/$t.out"; do
			sed -e "s#@config_file@#$CONFIG#g" -e "s#/home/gpadmin/s3.conf#$CONFIG#g" \
				-e "s#@read_prefix@#s3test.pivotal.io/regress#g" \
				-e "s#@write_prefix@#s3test.pivotal.io/regress/$write#g" \
				-e "s#@write_encrypt_prefix@#s3test.encrypt.pivotal.io/regress/$write#g" \
				-e "s#@abs_srcdir@#$src#g" "$GPCLOUD/regress/${f%%:*}" > "$src/${f#*:}"
		done
	done
	# Cloudberry's schedule, its groups kept, less the tests the manifest skips
	awk -v tests=" $(echo $run_tests) " '
		/^test:/ {
			line = ""
			for (i = 2; i <= NF; i++)
				if (index(tests, " " $i " ")) line = line " " $i
			if (line != "") print "test:" line
		}' "$GPCLOUD/regress/regress_schedule" > "$src/schedule"
}

mkdir -p "$ROOT/gpdiff"
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$ROOT/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$ROOT/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, with a difference reviewed and kept
# counted as none (see the diskquota suite); <test>.<target>.diff is one
# target's alone.
cat > "$EXEC/bin/diff" <<EOF2
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans --gpd_init "$CB/init_file"
    --gpd_init "$HERE/../greenplum/init_file" --gpd_init "$HERE/init_file")
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

# the servers: a cluster, as the greenplum suite makes one, and one node
CONF="$ROOT/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
done > "$CONF"
start_nodes() {				# start_nodes <node...>
	local n
	for n in "$@"; do
		mkdir -p "$(sockdir "$n")"
		"$BINDIR/initdb" -D "$(datadir "$n")" -N -U gpadmin --locale=C --encoding=UTF8 \
			> "$ROOT/initdb$n.log" 2>&1 \
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
	for n in "$@"; do
		PATH="$EXEC/bin:$PATH" "$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 60 start \
			> /dev/null 2>&1 || { echo "node $n did not start"; tail -20 "$ROOT/node$n.log"; exit 1; }
	done
}

# regress <target> <node>
regress() {
	local target="$1" node="$2" out="$ROOT/out-$1" src="$ROOT/source_replaced-$1" total bad
	mkdir -p "$ROOT/canon-$target" "$out"
	source_replaced "$target"
	( cd "$src" && TARGET="$target" PATH="$EXEC/bin:$PATH" "$PG_REGRESS" \
		--bindir="$BINDIR" \
		--inputdir="$src" \
		--expecteddir="$src" \
		--outputdir="$out" \
		--schedule="$src/schedule" \
		--load-extension=gp_core \
		--load-extension=gp_sql \
		--load-extension=gp_exttable \
		--dbname=regression \
		--host="$(sockdir "$node")" --port="$(port "$node")" ) > "$out/pg_regress.out" 2>&1
	grep -E "^(not )?ok " "$out/pg_regress.out" | sed 's/^/  /'
	total=$(grep -cE "^(not )?ok " "$out/pg_regress.out")
	bad=$(grep -cE "^not ok " "$out/pg_regress.out")
	if [ "$total" -gt 0 ] && [ "$bad" -eq 0 ]; then
		ok "Cloudberry's schedule, $target: $total of $total"
	else
		notok "Cloudberry's schedule, $target: $((total - bad)) of $total" \
			"$(ls "$ROOT/canon-$target" 2> /dev/null | sed 's/\.diff$//' | tr '\n' ' ')"
		if [ -n "${RESULTS_DIR:-}" ]; then
			cp "$out/regression.diffs" "$RESULTS_DIR/gpcloud-$target.diffs" 2> /dev/null
			mkdir -p "$RESULTS_DIR/gpcloud-$target-canon"
			cp "$ROOT/canon-$target"/*.diff "$RESULTS_DIR/gpcloud-$target-canon/" 2> /dev/null
		fi
	fi
}

# own <target> <node>: the suite's checks of what Cloudberry's tests would
# check with its data sets, which the suite does not make
own() {
	local target="$1" node="$2" loc="s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/regress/port" out
	q "$node" postgres "CREATE DATABASE own" > /dev/null
	out=$(q "$node" own "
		SET client_min_messages = warning;
		CREATE EXTENSION IF NOT EXISTS gp_core;
		CREATE EXTENSION IF NOT EXISTS gp_sql;
		CREATE EXTENSION gp_exttable;
		CREATE FUNCTION read_from_s3() RETURNS integer AS '\$libdir/gpcloud.so', 's3_import' LANGUAGE C STABLE;
		CREATE FUNCTION write_to_s3() RETURNS integer AS '\$libdir/gpcloud.so', 's3_export' LANGUAGE C STABLE;
		CREATE PROTOCOL s3 (readfunc = read_from_s3, writefunc = write_to_s3);
		CREATE READABLE EXTERNAL TABLE gz (date text, time text, open float, high float, low float, volume int)
			LOCATION('$loc/gzip/ config=$CONFIG') FORMAT 'csv';
		CREATE READABLE EXTERNAL TABLE deflated (date text, time text, open float, high float, low float, volume int)
			LOCATION('$loc/deflate/ config=$CONFIG') FORMAT 'csv';
		CREATE READABLE EXTERNAL TABLE chunks (date text, time text, open float, high float, low float, volume int)
			LOCATION('$loc/chunks/ config=$CONFIG') FORMAT 'csv';
		CREATE READABLE EXTERNAL TABLE thousands (date text, time text, open float, high float, low float, volume int)
			LOCATION('s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/regress/2001files/ config=$CONFIG') FORMAT 'csv';")
	[ -z "$out" ] && ok "$target: the protocol and the tables made, in a database of the suite's" \
		|| notok "$target: the protocol and the tables" "$out"
	for t in gz deflated; do
		out=$(q "$node" own "SELECT count(*), round(sum(open)) FROM $t")
		[ "$out" = "117446|4239338" ] && ok "$target: small17/data0000 ${t/gz/gzipped}, read as it is plain" \
			|| notok "$target: the $t object" "$out"
	done
	out=$(q "$node" own "SELECT count(*), sum(open), min(volume), max(volume) FROM chunks")
	[ "$out" = "800000|800000|100|800099" ] && ok "$target: 36 MB, read by gpcloud's threads a chunk each" \
		|| notok "$target: the object of three chunks" "$out"
	if [ "$target" = cluster ]; then
		out=$(q "$node" own "SELECT string_agg(n::text, ',' ORDER BY s) FROM
			(SELECT gp_segment_id s, count(*) n FROM thousands GROUP BY 1) x")
		[ "$out" = "85376,85376,85376" ] &&
			ok "$target: the 2,001 objects shared among the segments, each reading its keys" ||
			notok "$target: the segments' shares of 2001files" "$out"
	fi
}

for target in $TARGETS; do
	case "$target" in
		cluster)
			start_nodes 1 2 3 0
			regress cluster 0
			own cluster 0
			for n in $NODES; do "$BINDIR/pg_ctl" -D "$(datadir "$n")" -m fast -w stop > /dev/null 2>&1; done ;;
		one_node)
			start_nodes 9
			regress one_node 9
			own one_node 9
			"$BINDIR/pg_ctl" -D "$(datadir 9)" -m fast -w stop > /dev/null 2>&1 ;;
	esac
done

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
