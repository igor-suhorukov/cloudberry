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
#
# M2: the cluster.
#
# Three servers in one container -- a coordinator and two segments -- which is
# the smallest thing that can be got wrong in an interesting way.  What this
# checks is that each of them knows which node it is, that they agree about the
# cluster, and that a cluster described wrongly is a server that does not start
# rather than one that dispatches somewhere unexpected.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/cluster/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"

PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DYLD_LIBRARY_PATH="$PG_LIBDIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-cluster-XXXXXX")"
BASEPORT="${PGPORT:-$((6100 + RANDOM % 300))}"
CONF="$ROOT/gp_cluster.conf"

PRELOAD='gp_core,gp_sql'

pass=0; fail=0
isnum() { [[ "$1" =~ ^[0-9]+$ ]]; }
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; fail=$((fail + 1)); }

# node <n>: 0 is the coordinator, 1..n the segments.
datadir() { echo "$ROOT/node$1/data"; }
sockdir() { echo "$ROOT/node$1/sock"; }
port()    { echo $((BASEPORT + $1)); }
dbid()    { echo $(($1 + 1)); }
content() { [ "$1" -eq 0 ] && echo -1 || echo $(($1 - 1)); }

cleanup() {
	for n in 0 1 2; do
		"$BINDIR/pg_ctl" -D "$(datadir $n)" -m immediate stop >/dev/null 2>&1
	done
	rm -rf "$ROOT"
}
trap cleanup EXIT

start_node() {				# start_node <n> [extra postgresql.auto.conf lines]
	local n="$1"; shift
	local d; d="$(datadir "$n")"

	"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "gp.cluster_config = '$CONF'"
		echo "gp.dbid = $(dbid "$n")"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		for line in "$@"; do echo "$line"; done
	} > "$d/postgresql.auto.conf"
	"$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node$n.log" -w -t 30 start >/dev/null 2>&1
}

q() {						# q <n> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres \
		-c "$2" 2>&1
}

# Several statements in one session, which is one gang: the connections to the
# segments are the session's, so what one statement leaves behind is what the
# next one finds.
qf() {						# qf <n>, statements on stdin
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d postgres \
		-f - 2>&1
}

echo "M2 cluster tests"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

for n in 0 1 2; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 \
		> "$ROOT/initdb$n.log" 2>&1
	if [ $? -ne 0 ]; then
		echo "initdb failed for node $n:"; tail -20 "$ROOT/initdb$n.log"; exit 1
	fi
done

# The cluster, as every node reads it.  A host that starts with "/" is a
# directory holding a socket, which is what libpq makes of it; a cluster spread
# over machines writes host names here instead.
{
	echo "# dbid content role host port datadir"
	for n in 0 1 2; do
		echo "$(dbid "$n") $(content "$n") p $(sockdir "$n") $(port "$n") $(datadir "$n")"
	done
} > "$CONF"

###############################################################################
echo "1. every node starts and knows which one it is"
###############################################################################
started=1
for n in 1 2 0; do
	if start_node "$n"; then
		ok "node $n starts"
	else
		notok "node $n starts" "$(tail -5 "$ROOT/node$n.log")"
		started=0
	fi
done

if [ "$started" -eq 1 ]; then
	# Once, on the coordinator: CREATE EXTENSION is DDL, and DDL is dispatched.
	out=$(q 0 "CREATE EXTENSION gp_core;")
	[ -z "$out" ] && ok "CREATE EXTENSION gp_core, on the coordinator alone" \
		|| notok "CREATE EXTENSION gp_core" "$out"

	out=$(q 0 "SELECT role, segments, content_id, single_node, dbid FROM gp.node();")
	[ "$out" = "dispatch|2|-1|f|1" ] \
		&& ok "the coordinator dispatches, over 2 segments ($out)" \
		|| notok "gp.node() on the coordinator" "$out"

	# A psql opened on a segment is a utility session, as it is in Cloudberry:
	# the node executes what it is sent, but this connection was not sent.
	out=$(q 1 "SELECT role, segments, content_id, single_node, dbid FROM gp.node();")
	[ "$out" = "utility|2|0|f|2" ] \
		&& ok "a direct connection to segment 0 is a utility session ($out)" \
		|| notok "gp.node() on segment 0" "$out"

	out=$(q 2 "SELECT content_id, dbid FROM gp.node();")
	[ "$out" = "1|3" ] && ok "segment 1 knows its content id and dbid" \
		|| notok "gp.node() on segment 1" "$out"

	###########################################################################
	echo "2. they agree about the cluster, and it is readable as rows"
	###########################################################################
	out=$(q 0 "SELECT count(*), sum(content), count(*) FILTER (WHERE role = 'p') FROM gp.segment_configuration();")
	[ "$out" = "3|0|3" ] \
		&& ok "gp.segment_configuration() lists the three nodes" \
		|| notok "gp.segment_configuration()" "$out"

	out=$(q 0 "SELECT content || ' ' || port FROM gp.segment_configuration() WHERE content >= 0 ORDER BY content;")
	want="0 $(port 1)
1 $(port 2)"
	[ "$out" = "$want" ] && ok "the segments are in content order, with their ports" \
		|| notok "segment ports" "$out"

	out=$(q 1 "SELECT count(*) FROM gp.segment_configuration();")
	[ "$out" = "3" ] && ok "a segment reads the same file" \
		|| notok "gp.segment_configuration() on a segment" "$out"

	###########################################################################
	echo "3. the coordinator reaches the segments"
	###########################################################################
	out=$(q 0 "SELECT content || '=' || result FROM gp.exec_on_segments('SELECT content_id FROM gp.node()') ORDER BY content;")
	[ "$out" = "0=0
1=1" ] && ok "every segment answers, and each one for itself" \
		|| notok "gp.exec_on_segments()" "$out"

	# The identity is what makes the backend on the other end a segment
	# process rather than somebody's psql, and this is where that is decided.
	out=$(q 0 "SELECT DISTINCT result FROM gp.exec_on_segments('SELECT role FROM gp.node()');")
	[ "$out" = "execute" ] \
		&& ok "a dispatched backend executes, where a psql on the same node is utility" \
		|| notok "the role of a dispatched backend" "$out"

	out=$(q 0 "SELECT result FROM gp.exec_on_segments('SELECT current_setting(''gp.qe_identity'')') WHERE content = 0;")
	case "$out" in
		seg0/dbid1/sess*) ok "and carries the identity the coordinator gave it ($out)" ;;
		*) notok "gp.qe_identity on a segment" "$out" ;;
	esac

	out=$(q 0 "SELECT DISTINCT result FROM gp.exec_on_segments('SELECT current_database() || '' as '' || current_user');")
	[ "$out" = "postgres as $(whoami)" ] \
		&& ok "in the same database, as the session's own user ($out)" \
		|| notok "database and user on a segment" "$out"

	# An error on one segment is this session's error, with its SQLSTATE and
	# the segment it came from -- and the other segment is waited for first,
	# so the connections are left usable.
	out=$(printf '%s\n' \
		"SELECT gp.exec_on_segments('SELECT 1 / content_id FROM gp.node()');" \
		"SELECT count(*) FROM gp.exec_on_segments('SELECT 1');" | qf 0)
	case "$out" in
		*"division by zero"*"segment 0"*"2"*)
			ok "an error on one segment is raised here, naming it" ;;
		*) notok "an error on a segment" "$out" ;;
	esac

	out=$(printf '%s\n' \
		"\\set ON_ERROR_STOP off" \
		"SELECT gp.exec_on_segments('SELECT 1/0');" \
		"SELECT sqlstate FROM (SELECT 1) t, LATERAL (SELECT '22012'::text AS sqlstate) s;" | qf 0)
	case "$out" in *"division by zero"*) ok "and it is the segment's own message" ;;
		*) notok "the segment's message" "$out" ;; esac

	# The gang is the session's: a second statement uses the same connections,
	# which is why the identity above holds a session id.
	out=$(printf '%s\n' \
		"SELECT count(*) FROM gp.exec_on_segments('SELECT pg_backend_pid()');" \
		"SELECT count(DISTINCT result) FROM gp.exec_on_segments('SELECT pg_backend_pid()');" | qf 0)
	[ "$out" = "2
2" ] && ok "the connections are the session's, and there are two of them" \
		|| notok "the gang's connections" "$out"

	# Every node reads the same file, so a segment knows where the other
	# segments are -- and would dispatch to them, and to itself, if nothing
	# said otherwise.  Only the coordinator dispatches.
	out=$(q 1 "SELECT count(*) FROM gp.exec_on_segments('SELECT 1');")
	case "$out" in
		*"only the coordinator dispatches"*) ok "a segment does not dispatch, though it could reach the others" ;;
		*) notok "dispatch from a segment" "$out" ;;
	esac

	###########################################################################
	echo "4. a relation's rows, as the segments hold them"
	###########################################################################
	# gp.dist_random() is Cloudberry's gp_dist_random: the rows of a relation
	# from every segment, with nothing planned around it.  The rows are put on
	# the segments by hand, because routing an INSERT is a later step.
	q 0 "CREATE TABLE t (a int, b text);" >/dev/null
	q 0 "SELECT gp.exec_on_segments('INSERT INTO t SELECT g, ''row'' || g FROM generate_series(1, 3) g');" >/dev/null

	out=$(printf '%s\n' "SELECT count(*), sum(a), min(b), max(b) FROM gp.dist_random(NULL::t);" | qf 0)
	[ "$out" = "6|12|row1|row3" ] \
		&& ok "every segment's rows arrive, and only theirs ($out)" \
		|| notok "gp.dist_random()" "$out"

	out=$(q 0 "SELECT count(*) FROM t;")
	[ "$out" = "6" ] && ok "and a plain SELECT gathers the same rows" \
		|| notok "SELECT of a distributed table" "$out"

	# The binary path is the one that runs for types with a send function; a
	# type that has none makes the whole result text, which is why both are
	# worth a row here.
	q 0 "CREATE TABLE tt (a numeric, b timestamptz, c point, d int[]);" >/dev/null
	q 0 "SELECT gp.exec_on_segments('INSERT INTO tt VALUES (1.25, ''2026-09-22 10:00:00+00'', ''(1,2)'', ARRAY[1,2,3])');" >/dev/null
	out=$(printf '%s\n' "SET timezone = 'UTC';" "SELECT a, b, c, d FROM gp.dist_random(NULL::tt) LIMIT 1;" | qf 0)
	[ "$out" = "1.25|2026-09-22 10:00:00+00|(1,2)|{1,2,3}" ] \
		&& ok "values come back as themselves, through the binary path ($out)" \
		|| notok "gp.dist_random() of several types" "$out"

	out=$(q 0 "SELECT count(*) FROM gp.dist_random(NULL::int);")
	case "$out" in
		*"not a relation's row type"*) ok "and it has to be given a relation" ;;
		*) notok "gp.dist_random(NULL::int)" "$out" ;;
	esac

	###########################################################################
	echo "5. DDL reaches every node, with the coordinator's OIDs"
	###########################################################################
	# same_everywhere <what> <query>: the query's answer on the coordinator,
	# and on each segment through the dispatcher, all the same.
	same_everywhere() {
		local what="$1" sql="$2" here there
		here=$(q 0 "$sql")
		there=$(q 0 "SELECT string_agg(coalesce(result, '<null>'), ' ' ORDER BY content) FROM gp.exec_on_segments(\$gp\$$sql\$gp\$);")
		if [ -n "$here" ] && [ "$there" = "$here $here" ]; then
			ok "$what ($here)"
		else
			notok "$what" "coordinator: $here; segments: $there"
		fi
	}

	same_everywhere "the extension was made on the segments, its functions under the same OIDs" \
		"SELECT 'gp.node()'::regprocedure::oid::text"

	q 0 "CREATE TABLE d1 (a int PRIMARY KEY, b serial, c text DEFAULT 'x') ;" >/dev/null
	same_everywhere "a table has the same OID on every node" \
		"SELECT 'd1'::regclass::oid::text"
	same_everywhere "and so do its row type, its index, its sequence and its TOAST table" \
		"SELECT string_agg(o::text, ',') FROM (SELECT reltype AS o FROM pg_class WHERE oid = 'd1'::regclass UNION ALL SELECT 'd1_pkey'::regclass::oid UNION ALL SELECT 'd1_b_seq'::regclass::oid UNION ALL SELECT reltoastrelid FROM pg_class WHERE oid = 'd1'::regclass) s"

	q 0 "ALTER TABLE d1 ADD COLUMN e int; CREATE INDEX d1_c ON d1 (c); CREATE VIEW v1 AS SELECT a FROM d1;" >/dev/null
	same_everywhere "ALTER TABLE, CREATE INDEX and CREATE VIEW follow, in one string of statements" \
		"SELECT string_agg(attname, ',' ORDER BY attnum) || ' ' || 'd1_c'::regclass::oid || ' ' || 'v1'::regclass::oid FROM pg_attribute WHERE attrelid = 'd1'::regclass AND attnum > 0"

	q 0 "CREATE FUNCTION f1(int) RETURNS int LANGUAGE sql AS 'SELECT \$1 + 1'; CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');" >/dev/null
	same_everywhere "a function and an enum, its labels' OIDs included" \
		"SELECT 'f1(int)'::regprocedure::oid || ' ' || (SELECT string_agg(oid::text, ',' ORDER BY enumsortorder) FROM pg_enum WHERE enumtypid = 'mood'::regtype)"

	# The name is looked up where search_path says, there as here.
	q 0 "CREATE SCHEMA s1;" >/dev/null
	printf '%s\n' "SET search_path = s1;" "CREATE TABLE in_s1 (a int);" | qf 0 >/dev/null
	same_everywhere "search_path is the segments' too" \
		"SELECT 's1.in_s1'::regclass::oid::text"

	# gp_sql makes each partition as a statement of its own, with the
	# parent's text; what travels is the tree, so each one arrives as itself.
	q 0 "CREATE EXTENSION gp_sql;" >/dev/null
	q 0 "CREATE TABLE pt (a int, d int) PARTITION BY RANGE (d) (START (1) END (4) EVERY (1));" >/dev/null
	same_everywhere "a classic partitioned table: gp_sql's partitions, made once each" \
		"SELECT string_agg(c.relname || '=' || c.oid, ',' ORDER BY c.relname) FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid WHERE i.inhparent = 'pt'::regclass"

	q 0 "DROP VIEW v1; DROP TABLE d1;" >/dev/null
	same_everywhere "DROP follows" \
		"SELECT count(*) FROM pg_class WHERE relname IN ('d1', 'v1', 'd1_pkey')"

	###########################################################################
	echo "6. the segments' work is part of the coordinator's transaction"
	###########################################################################
	printf '%s\n' "BEGIN;" "CREATE TABLE gone (a int);" "ROLLBACK;" | qf 0 >/dev/null
	same_everywhere "BEGIN; CREATE TABLE; ROLLBACK leaves no table anywhere" \
		"SELECT count(*) FROM pg_class WHERE relname = 'gone'"

	printf '%s\n' "BEGIN;" "CREATE TABLE kept (a int);" "SAVEPOINT s;" \
		"CREATE TABLE undone (a int);" "ROLLBACK TO SAVEPOINT s;" \
		"CREATE TABLE kept2 (a int);" "COMMIT;" | qf 0 >/dev/null
	same_everywhere "a savepoint rolled back here is rolled back there" \
		"SELECT string_agg(relname, ',' ORDER BY relname) FROM pg_class WHERE relname IN ('kept', 'undone', 'kept2')"

	# A PL/pgSQL exception block is a subtransaction too.
	out=$(printf '%s\n' "DO \$\$ BEGIN CREATE TABLE in_block (a int); PERFORM 1/0; EXCEPTION WHEN division_by_zero THEN CREATE TABLE after_block (a int); END \$\$;" | qf 0)
	same_everywhere "DDL inside a function is dispatched, and its exception block unwinds there too" \
		"SELECT string_agg(relname, ',' ORDER BY relname) FROM pg_class WHERE relname IN ('in_block', 'after_block')"

	# A statement that fails on one segment only fails here, and is undone
	# everywhere: segment 0 already has a table by that name, made in a
	# utility session behind the coordinator's back.
	q 1 "CREATE TABLE clash (a int);" >/dev/null
	out=$(q 0 "CREATE TABLE clash (a int);")
	case "$out" in
		*"already exists"*"segment 0"*) ok "a statement that fails on one segment fails here, naming it" ;;
		*) notok "a failure on one segment" "$out" ;;
	esac
	out=$(q 0 "SELECT count(*) FROM pg_class WHERE relname = 'clash';")
	out2=$(q 2 "SELECT count(*) FROM pg_class WHERE relname = 'clash';")
	[ "$out|$out2" = "0|0" ] && ok "and is undone on the coordinator and on the other segment" \
		|| notok "the failed statement's other nodes" "coordinator $out, segment 1 $out2"
	q 1 "DROP TABLE clash;" >/dev/null

	###########################################################################
	echo "7. databases, roles and temporary tables"
	###########################################################################
	# CREATE DATABASE cannot run in a transaction block, so each segment runs
	# it in one of its own.
	out=$(q 0 "CREATE DATABASE db2;")
	[ -z "$out" ] && ok "CREATE DATABASE runs" || notok "CREATE DATABASE" "$out"
	same_everywhere "and the database has one OID everywhere" \
		"SELECT oid::text FROM pg_database WHERE datname = 'db2'"
	q 0 "DROP DATABASE db2;" >/dev/null
	same_everywhere "DROP DATABASE follows" \
		"SELECT count(*) FROM pg_database WHERE datname = 'db2'"

	q 0 "CREATE ROLE r1 NOLOGIN; CREATE TABLE owned (a int); ALTER TABLE owned OWNER TO r1;" >/dev/null
	same_everywhere "a role, and a table it was given" \
		"SELECT 'r1'::regrole::oid || ' ' || pg_get_userbyid(relowner) FROM pg_class WHERE relname = 'owned'"

	# Each backend makes its own temporary namespace, so its OID is the one
	# thing that differs; the tables in it do not.
	out=$(printf '%s\n' "SET client_min_messages = warning;" "CREATE TEMP TABLE tmp1 (a int);" "CREATE TEMP TABLE tmp2 (a int);" \
		"SELECT ('tmp1'::regclass::oid = min(result::oid)) AND ('tmp1'::regclass::oid = max(result::oid)) AND count(*) = 2 FROM gp.exec_on_segments('SELECT ''tmp1''::regclass::oid');" \
		"SELECT ('tmp2'::regclass::oid = min(result::oid)) AND count(*) = 2 FROM gp.exec_on_segments('SELECT ''tmp2''::regclass::oid');" | qf 0)
	[ "$out" = "t
t" ] && ok "temporary tables, two of them, have the coordinator's OIDs" \
		|| notok "temporary tables" "$out"

	out=$(q 0 "VACUUM kept;")
	[ -z "$out" ] && ok "VACUUM, which runs outside a transaction block, reaches the segments too" \
		|| notok "VACUUM" "$out"

	# A temporary table is in the session's own temporary schema, which on the
	# coordinator and on each segment is a different pg_temp_N: here another
	# session holds the coordinator's first backend slot, which no segment
	# backend has, so the numbers differ, and "pg_temp" is what names both.
	"$PSQL" -X -q -h "$(sockdir 0)" -p "$(port 0)" -d postgres -c "SELECT pg_sleep(4)" >/dev/null 2>&1 &
	holder=$!
	sleep 1
	out=$(printf '%s\n' "SET client_min_messages = warning;" \
		"CREATE TEMP TABLE tmp_num (a int, b int);" \
		"INSERT INTO tmp_num SELECT i, i FROM generate_series(1, 10) i;" \
		"UPDATE tmp_num SET b = b + 1 WHERE a = 1;" \
		"SELECT count(*), sum(b), (SELECT nspname FROM pg_namespace WHERE oid = pg_my_temp_schema()) <> (SELECT min(r.result) FROM gp.exec_on_segments('SELECT nspname::text FROM pg_namespace WHERE oid = pg_my_temp_schema()') r) FROM tmp_num;" | qf 0)
	wait "$holder"
	[ "$out" = "10|56|t" ] && ok "a temporary table read and written on the segments, whose temporary schemas are named otherwise" \
		|| notok "temporary schemas named otherwise on the segments" "$out"

	###########################################################################
	echo "8. a distributed table's rows live on the segments"
	###########################################################################
	# Cloudberry's own NOTICE, word for word, 345 of its expected outputs hold it.
	out=$(q 0 "CREATE TABLE d (a int, b text);" 2>&1)
	case "$out" in
		*"Table doesn't have 'DISTRIBUTED BY' clause -- Using column named 'a' as the Apache Cloudberry data distribution key for this table."*"The 'DISTRIBUTED BY' clause determines the distribution of data. Make sure column(s) chosen are the optimal data distribution key to minimize skew."*)
			ok "a table nobody distributed is distributed by its first column, and says so" ;;
		*) notok "the default distribution's NOTICE" "$out" ;;
	esac

	out=$(q 0 "CREATE TABLE pk (x text, y int PRIMARY KEY);" 2>&1)
	out2=$(q 0 "SELECT kind || ' ' || array_to_string(columns, ',') FROM gp.policy('pk');")
	[ -z "$out" ] && [ "$out2" = "hash y" ] \
		&& ok "a table with a primary key is distributed by it, silently" \
		|| notok "the primary key's distribution" "$out / $out2"

	out=$(q 0 "INSERT INTO d SELECT g, 'row' || g FROM generate_series(1, 100) g;")
	[ -z "$out" ] && ok "INSERT ... SELECT" || notok "INSERT ... SELECT" "$out"

	n1=$(q 1 "SELECT count(*) FROM d;"); n2=$(q 2 "SELECT count(*) FROM d;")
	isnum "$n1" && isnum "$n2" && [ "$((n1 + n2))" = "100" ] && [ "$n1" -gt 0 ] && [ "$n2" -gt 0 ] \
		&& ok "the rows are on the segments, spread over both ($n1 and $n2)" \
		|| notok "where the rows are" "segment 0: $n1, segment 1: $n2"

	# Where each row went, checked against Cloudberry's arithmetic written out
	# again, independently: hashint4 of the key, reduced to a segment by jump
	# consistent hashing.  The function is DDL, so the segments have it too.
	cat > "$ROOT/expected_seg.sql" <<'EOF'
CREATE FUNCTION expected_seg(v int, n int) RETURNS int
LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	key numeric := hashint4(v)::bigint & 4294967295;
	b bigint := -1;
	j bigint := 0;
BEGIN
	WHILE j < n LOOP
		b := j;
		key := mod(key * 2862933555777941757 + 1, 18446744073709551616);
		j := trunc((b + 1)::float8 * (2147483648::float8 / (floor(key / 8589934592) + 1)::float8));
	END LOOP;
	RETURN b;
END $$;
EOF
	qf 0 < "$ROOT/expected_seg.sql" >/dev/null
	out=$(q 1 "SELECT count(*) FROM d WHERE expected_seg(a, 2) <> 0;")
	out2=$(q 2 "SELECT count(*) FROM d WHERE expected_seg(a, 2) <> 1;")
	[ "$out|$out2" = "0|0" ] \
		&& ok "and each one is on the segment Cloudberry's hash puts it on" \
		|| notok "the rows' placement" "misplaced on segment 0: $out, on segment 1: $out2"

	out=$(q 0 "SELECT count(*), sum(a), min(b), max(a) FROM d;")
	[ "$out" = "100|5050|row1|100" ] && ok "SELECT gathers them back ($out)" \
		|| notok "SELECT from a distributed table" "$out"

	out=$(q 0 "EXPLAIN (COSTS OFF) SELECT b FROM d WHERE a = 7;")
	out2=$(q 0 "SELECT b FROM d WHERE a = 7;")
	case "$out" in
		*"Gather Motion 1:1 on d"*"(slice1; segments: 1)"*)
			[ "$out2" = "row7" ] && ok "a key fixed to a constant asks one segment (direct dispatch)" \
				|| notok "direct dispatch's answer" "$out2" ;;
		*) notok "direct dispatch" "$out" ;;
	esac

	out=$(q 0 "EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM d WHERE a < 10 AND b <> now()::text;")
	case "$out" in
		*"Filter: (d.b <> (now())::text)"*"Remote SQL: SELECT NULL, b FROM ONLY public.d WHERE (a < 10)"*)
			ok "an immutable condition is evaluated on the segments, now() here, and only b is fetched" ;;
		*) notok "which conditions are sent" "$out" ;;
	esac
	out=$(q 0 "SELECT count(*) FROM d WHERE a < 10 AND b <> now()::text;")
	[ "$out" = "9" ] && ok "and the answer is the same ($out)" || notok "a sent condition's answer" "$out"

	q 0 "CREATE TABLE d2 (k int, v int) DISTRIBUTED BY (v); INSERT INTO d2 SELECT g, g % 7 FROM generate_series(1, 100) g;" >/dev/null 2>&1
	out=$(q 0 "SELECT count(*) FROM d JOIN d2 ON d.a = d2.k WHERE d2.v = 3;")
	[ "$out" = "14" ] && ok "a join of two tables distributed differently ($out)" \
		|| notok "a join of two distributed tables" "$out"

	out=$(q 0 "UPDATE d SET b = 'changed' WHERE a <= 10;")
	out2=$(q 0 "SELECT count(*) FROM d WHERE b = 'changed';")
	[ -z "$out" ] && [ "$out2" = "10" ] && ok "UPDATE is sent to the segments, and each changes its own rows" \
		|| notok "UPDATE" "$out / $out2"

	out=$(printf '%s\n' "UPDATE d SET b = 'x' WHERE a <= 10;" | "$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d postgres 2>&1)
	[ "$out" = "UPDATE 10" ] && ok "and its command tag counts every segment's rows" \
		|| notok "UPDATE's command tag" "$out"

	out=$(q 0 "UPDATE d SET a = a + 1000 WHERE a = 1;")
	case "$out" in
		*"a column of the distribution key"*) ok "an UPDATE of the key, which would move the row, is refused with the reason" ;;
		*) notok "an UPDATE of the distribution key" "$out" ;;
	esac

	out=$(q 0 "UPDATE d SET b = 'j' FROM d2 WHERE d.a = d2.k;")
	case "$out" in
		*"another distributed table"*) ok "and so is one that joins another distributed table" ;;
		*) notok "an UPDATE joining a distributed table" "$out" ;;
	esac

	out=$(printf '%s\n' "DELETE FROM d WHERE a > 90;" | "$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d postgres 2>&1)
	out2=$(q 0 "SELECT count(*) FROM d;")
	[ "$out|$out2" = "DELETE 10|90" ] && ok "DELETE" || notok "DELETE" "$out / $out2"

	printf '%s\n' "BEGIN;" "INSERT INTO d VALUES (500, 'gone');" "ROLLBACK;" | qf 0 >/dev/null
	out=$(q 0 "SELECT count(*) FROM d WHERE a = 500;")
	[ "$out" = "0" ] && ok "a rolled-back INSERT leaves nothing on the segments" \
		|| notok "a rolled-back INSERT" "$out"

	out=$(printf '%s\n' "BEGIN;" "INSERT INTO d VALUES (600, 'mine');" \
		"SELECT b FROM d WHERE a = 600;" "SELECT count(*) FROM d LIMIT 1;" "SELECT b FROM d WHERE a = 600;" "COMMIT;" | qf 0)
	[ "$out" = "mine
91
mine" ] && ok "a transaction reads its own rows, and a LIMIT leaves the connections usable" \
		|| notok "reading inside a transaction" "$out"

	# serial: one sequence, the coordinator's, whatever segment the row goes to
	q 0 "CREATE TABLE ser (id serial, v text); INSERT INTO ser (v) SELECT 'v' || g FROM generate_series(1, 20) g;" >/dev/null 2>&1
	out=$(q 0 "SELECT count(DISTINCT id), min(id), max(id) FROM ser;")
	[ "$out" = "20|1|20" ] && ok "a serial column counts once, on the coordinator ($out)" \
		|| notok "a serial column" "$out"

	q 0 "CREATE TABLE rep (k int, v text) DISTRIBUTED REPLICATED; INSERT INTO rep VALUES (1, 'a'), (2, 'b'), (3, 'c');" >/dev/null 2>&1
	n1=$(q 1 "SELECT count(*) FROM rep;"); n2=$(q 2 "SELECT count(*) FROM rep;")
	out=$(q 0 "SELECT count(*) FROM rep;")
	[ "$n1|$n2|$out" = "3|3|3" ] && ok "a replicated table: every segment holds every row, and it is read once" \
		|| notok "a replicated table" "$n1|$n2|$out"
	out=$(q 0 "UPDATE rep SET v = 'z' WHERE k = 2;")
	n1=$(q 1 "SELECT v FROM rep WHERE k = 2;"); n2=$(q 2 "SELECT v FROM rep WHERE k = 2;")
	[ "$n1|$n2" = "z|z" ] && ok "and an UPDATE of it reaches every copy" \
		|| notok "UPDATE of a replicated table" "$out $n1|$n2"
	out=$(printf '%s\n' "UPDATE rep SET v = 'y';" | "$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d postgres 2>&1)
	[ "$out" = "UPDATE 3" ] && ok "counted once, not once per segment" || notok "UPDATE count of a replicated table" "$out"

	out=$(q 0 "UPDATE d SET b = r.v FROM rep r WHERE d.a = r.k;")
	out2=$(q 0 "SELECT count(*) FROM d WHERE b = 'y';")
	[ -z "$out" ] && [ "$out2" = "3" ] && ok "an UPDATE that joins a replicated table is sent, each segment has it" \
		|| notok "UPDATE joining a replicated table" "$out / $out2"

	q 0 "CREATE TABLE rnd (a int, b text) DISTRIBUTED RANDOMLY; INSERT INTO rnd SELECT g, 'x' FROM generate_series(1, 200) g;" >/dev/null 2>&1
	n1=$(q 1 "SELECT count(*) FROM rnd;"); n2=$(q 2 "SELECT count(*) FROM rnd;")
	isnum "$n1" && isnum "$n2" && [ "$((n1 + n2))" = "200" ] && [ "$n1" -gt 20 ] && [ "$n2" -gt 20 ] \
		&& ok "a randomly distributed table spreads its rows ($n1 and $n2)" \
		|| notok "a random distribution" "$n1 and $n2"

	out=$(printf '%s\n' "COPY d FROM STDIN;" "700	copied" "701	copied" "702	copied" '\.' | qf 0)
	out2=$(q 0 "SELECT count(*) FROM d WHERE b = 'copied';")
	[ "$out2" = "3" ] && ok "COPY FROM routes its rows as INSERT does" \
		|| notok "COPY FROM" "$out / $out2"
	out=$(q 0 "COPY (SELECT a FROM d WHERE b = 'copied' ORDER BY a) TO STDOUT;")
	out2=$(q 0 "COPY d TO STDOUT;" | wc -l)
	[ "$out" = "700
701
702" ] && [ "$out2" = "94" ] && ok "COPY TO, of a query and of the table, gathers" \
		|| notok "COPY TO" "$out / $out2 lines"

	out=$(q 0 "SELECT count(*) FROM (SELECT a FROM d WHERE a < 5 FOR UPDATE) s;")
	[ "$out" = "4" ] && ok "SELECT ... FOR UPDATE locks the table, as Cloudberry does without GDD" \
		|| notok "SELECT FOR UPDATE" "$out"

	out=$(q 0 "INSERT INTO d VALUES (800, 'r') RETURNING a;")
	case "$out" in
		*"RETURNING into distributed table"*"not supported yet"*) ok "INSERT ... RETURNING is refused, for now, with the reason" ;;
		*) notok "INSERT RETURNING" "$out" ;;
	esac

	# A column dropped and one added: the positions the segments are told.
	q 0 "ALTER TABLE d2 DROP COLUMN k; ALTER TABLE d2 ADD COLUMN w text DEFAULT 'w';" >/dev/null
	q 0 "INSERT INTO d2 (v, w) VALUES (99, 'new');" >/dev/null
	out=$(q 0 "SELECT v, w FROM d2 WHERE v = 99;")
	[ "$out" = "99|new" ] && ok "a table with a dropped column writes and reads by name" \
		|| notok "a dropped column" "$out"

	q 0 "CREATE TABLE sales (id int, d date, amt int) DISTRIBUTED BY (id) PARTITION BY RANGE (d) (START (date '2026-01-01') END (date '2026-04-01') EVERY (interval '1 month'));" >/dev/null 2>&1
	q 0 "INSERT INTO sales SELECT g, date '2026-01-01' + (g % 90), g FROM generate_series(1, 90) g;" >/dev/null
	out=$(q 0 "SELECT count(*), sum(amt) FROM sales WHERE d >= date '2026-02-01';")
	out2=$(q 1 "SELECT count(*) FROM sales_1_prt_2;")
	[ "$out" = "59|3540" ] && isnum "$out2" && [ "$out2" -gt 0 ] \
		&& ok "a classic partitioned table: rows routed into its partitions, on the segments" \
		|| notok "a partitioned table" "$out / segment 0 partition 2: $out2"

	# CREATE TABLE AS: the table made on every node, distributed, then filled.
	q 0 "SET client_min_messages = warning; CREATE TABLE cta AS SELECT * FROM d WHERE a <= 50;" >/dev/null
	out=$(q 0 "SELECT gp_sql.distribution('cta'), count(*), sum(a) FROM cta;")
	n1=$(q 1 "SELECT count(*) FROM cta WHERE expected_seg(a, 2) <> 0;")
	n2=$(q 2 "SELECT count(*) FROM cta WHERE expected_seg(a, 2) <> 1;")
	s1=$(q 1 "SELECT count(*) FROM cta;")
	[ "$out|$n1|$n2" = "(a)|50|1275|0|0" ] && isnum "$s1" && [ "$s1" -gt 0 ] && [ "$s1" -lt 50 ] \
		&& ok "CREATE TABLE AS: distributed by its first column, each row where it hashes" \
		|| notok "CREATE TABLE AS" "$out / misplaced $n1 $n2 / segment 0 has $s1"
	q 0 "CREATE TABLE ctb (x, y) AS SELECT b, count(*) FROM d GROUP BY b DISTRIBUTED BY (y);" >/dev/null
	q 0 "SET client_min_messages = warning; CREATE TABLE ctc AS SELECT a FROM d WITH NO DATA;" >/dev/null
	out=$(q 0 "SELECT gp_sql.distribution('ctb'), count(*) = (SELECT count(DISTINCT b) FROM d) FROM ctb;")
	out2=$(q 0 "SELECT gp_sql.distribution('ctc'), count(*) FROM ctc;")
	[ "$out|$out2" = "(y)|t|(a)|0" ] \
		&& ok "... with its DISTRIBUTED BY, and WITH NO DATA" \
		|| notok "CREATE TABLE AS DISTRIBUTED BY, WITH NO DATA" "$out / $out2"

	# What the routed writes do not take is refused, never run here against
	# the coordinator's empty copy -- which crashed on a ctid from a segment.
	out=$(q 0 "WITH w AS (UPDATE d SET b = b WHERE a < 3 RETURNING *) SELECT count(*) FROM w;")
	case "$out" in
		*"cannot UPDATE distributed table \"d\" this way yet"*"data-modifying WITH query"*)
			ok "an UPDATE in a WITH query is refused, not run on the coordinator" ;;
		*) notok "a data-modifying WITH query" "$out" ;;
	esac
	out=$(q 0 "DELETE FROM sales WHERE amt = 1;")
	case "$out" in
		*"cannot DELETE FROM distributed table"*"partitions of a partitioned table"*)
			ok "so is a DELETE of a partitioned table's partitions" ;;
		*) notok "a DELETE of a partitioned table" "$out" ;;
	esac
	q 0 "CREATE TABLE sq (a int, v text) DISTRIBUTED BY (a);" >/dev/null
	out=$(q 0 "INSERT INTO sq SELECT 7, (SELECT max(b) FROM d); SELECT count(*), max(v) = (SELECT max(b) FROM d) FROM sq;")
	[ "$out" = "1|t" ] && ok "an INSERT whose row holds a scalar subquery keeps the subquery's initplan" \
		|| notok "INSERT with a scalar subquery" "$out"

	# ALTER TABLE ... SET DISTRIBUTED: the policy, and the rows moved with it.
	q 0 "CREATE TABLE sd (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	q 0 "INSERT INTO sd SELECT i, i % 7 FROM generate_series(1, 200) i;" >/dev/null
	q 0 "ALTER TABLE sd SET DISTRIBUTED BY (b);" >/dev/null
	out=$(q 0 "SELECT gp_sql.distribution('sd'), count(*), sum(a) FROM sd;")
	w1=$(q 1 "SELECT count(*) FROM sd WHERE expected_seg(b, 2) <> 0;")
	w2=$(q 2 "SELECT count(*) FROM sd WHERE expected_seg(b, 2) <> 1;")
	[ "$out|$w1|$w2" = "(b)|200|20100|0|0" ] \
		&& ok "SET DISTRIBUTED BY: every row moved to where the new key hashes" \
		|| notok "SET DISTRIBUTED BY" "$out / misplaced $w1 $w2"
	q 0 "ALTER TABLE sd SET DISTRIBUTED REPLICATED;" >/dev/null
	n1=$(q 1 "SELECT count(*) FROM sd;")
	q 0 "ALTER TABLE sd SET DISTRIBUTED RANDOMLY;" >/dev/null
	out=$(q 0 "SELECT gp_sql.distribution('sd'), count(*), sum(a) FROM sd;")
	[ "$n1|$out" = "200|random|200|20100" ] \
		&& ok "... REPLICATED puts every row on every segment, RANDOMLY back from it counts each once" \
		|| notok "SET DISTRIBUTED REPLICATED, RANDOMLY" "$n1 / $out"
	out=$(q 1 "ALTER TABLE sd SET DISTRIBUTED BY (a);")
	case "$out" in
		*"SET DISTRIBUTED BY not supported in utility mode"*) ok "... and a segment's utility session refuses it, as Cloudberry's does" ;;
		*) notok "SET DISTRIBUTED BY in a utility session" "$out" ;;
	esac

	out=$(printf '%s\n' "TRUNCATE d;" "SELECT count(*) FROM d;" | qf 0)
	n1=$(q 1 "SELECT count(*) FROM d;")
	[ "$out|$n1" = "0|0" ] && ok "TRUNCATE empties the segments" || notok "TRUNCATE" "$out|$n1"

	# gp_segment_id: Cloudberry's system column, which O10 lets the port give
	# the name to where no column has it -- a call of the row's segment,
	# which a segment answers for itself and ruleutils prints as the name.
	q 0 "CREATE TABLE gs (a int, b int) DISTRIBUTED BY (a); INSERT INTO gs SELECT i, 0 FROM generate_series(1, 100) i;" >/dev/null
	q 0 "CREATE TABLE gr (a int) DISTRIBUTED RANDOMLY; INSERT INTO gr SELECT generate_series(1, 100);" >/dev/null
	q 0 "CREATE TABLE gre (a int) DISTRIBUTED REPLICATED; INSERT INTO gre SELECT generate_series(1, 10);" >/dev/null
	n1=$(q 1 "SELECT count(*) FROM gs;"); n2=$(q 2 "SELECT count(*) FROM gs;")
	out=$(q 0 "SELECT gp_segment_id, count(*) FROM gs GROUP BY 1 ORDER BY 1;")
	[ "$out" = "0|$n1
1|$n2" ] && ok "gp_segment_id of a hashed table's rows is the segment that holds each" \
		|| notok "gp_segment_id in a target list" "$out (segments hold $n1 and $n2)"
	out=$(q 0 "SELECT count(*) FROM gs t WHERE t.gp_segment_id <> expected_seg(a, 2);")
	out2=$(q 0 "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM gs WHERE gp_segment_id = 1;")
	case "$out|$out2" in
		"0|"*"Remote SQL: SELECT a, NULL FROM ONLY public.gs WHERE (gp_segment_id = 1)"*)
			ok "t.gp_segment_id in a condition is sent to the segments, each answering for itself" ;;
		*) notok "gp_segment_id in a sent condition" "$out / $out2" ;;
	esac
	r1=$(q 1 "SELECT count(*) FROM gr;")
	out=$(q 0 "SELECT count(*) FROM gr WHERE gp_segment_id = 0;")
	out2=$(q 0 "SELECT gp_segment_id FROM gr LIMIT 1;")
	case "$out|$out2" in
		"$r1|"*"gp_segment_id of randomly distributed table \"gr\" is known only on its segments"*)
			ok "a random table's: in a condition the segments answer, here it is refused" ;;
		*) notok "gp_segment_id of a random table" "$out (segment 0 holds $r1) / $out2" ;;
	esac
	out=$(q 0 "SELECT count(DISTINCT gp_segment_id), min(gp_segment_id) IN (0, 1) FROM gre;")
	out2=$(q 0 "SELECT gp_segment_id, count(*) FROM gp.dist_random(NULL::gre) GROUP BY 1 ORDER BY 1;")
	[ "$out|$out2" = "1|t|0|10
1|10" ] && ok "a replicated table's is the segment it was read from; gp.dist_random() gives each copy's" \
		|| notok "gp_segment_id of a replicated table" "$out / $out2"
	out=$(q 0 "SELECT count(*) FROM gp.dist_random(NULL::gs) d WHERE d.gp_segment_id <> expected_seg(a, 2);")
	out2=$(q 0 "SELECT gp_segment_id, * FROM gp.dist_random(NULL::gs) WHERE a = 1;")
	[ "$out|$out2" = "0|$(q 0 "SELECT expected_seg(1, 2);")|1|0" ] \
		&& ok "gp.dist_random() of a hashed table: each row's segment, and \"*\" without it" \
		|| notok "gp_segment_id of gp.dist_random()" "$out / $out2"
	out=$(q 0 "SELECT gp_segment_id, count(*), sum(a) FROM gp_dist_random('gre') GROUP BY 1 ORDER BY 1;")
	out2=$(q 0 "SELECT count(*) FROM gp_dist_random('gs') WHERE gs.gp_segment_id <> expected_seg(gs.a, 2);")
	[ "$out|$out2" = "0|10|55
1|10|55|0" ] && ok "gp_dist_random('t'), as Cloudberry spells it, is gp.dist_random(NULL::t) named t" \
		|| notok "gp_dist_random('t')" "$out / $out2"
	out=$(q 0 "SELECT DISTINCT gp_segment_id FROM pg_class;")
	out2=$(q 0 "SELECT DISTINCT gp_segment_id FROM gp.dist_random(NULL::pg_namespace) ORDER BY 1;")
	[ "$out|$out2" = "-1|0
1" ] && ok "a catalog's is -1 here, and each segment's through gp.dist_random()" \
		|| notok "gp_segment_id of a catalog" "$out / $out2"
	out=$(q 0 "SELECT gp_segment_id FROM gs, gre;")
	out2=$(q 0 "SELECT gp_segment_id FROM (SELECT a FROM gs) s;")
	case "$out|$out2" in
		*"column reference \"gp_segment_id\" is ambiguous"*"column \"gp_segment_id\" does not exist"*)
			ok "two relations make it ambiguous, and a subquery has none, as in Cloudberry" ;;
		*) notok "where gp_segment_id is not one relation's" "$out / $out2" ;;
	esac
	q 0 "CREATE VIEW gsv AS SELECT gp_segment_id, a FROM gs WHERE gp_segment_id = 1;" >/dev/null
	out=$(q 0 "SELECT pg_get_viewdef('gsv');" | tr -s ' \n' ' '); out=${out% }
	v1=$(q 1 "SELECT count(*) FROM gsv;"); v2=$(q 2 "SELECT count(*) FROM gsv;")
	[ "$out|$v1|$v2" = " SELECT gp_segment_id, a FROM gs WHERE (gp_segment_id = 1);|0|$n2" ] \
		&& ok "a view prints it as the column, and each segment's copy of the view answers for itself" \
		|| notok "a view of gp_segment_id" "$out / $v1 $v2"
	out=$(q 0 "EXPLAIN (VERBOSE, COSTS OFF) SELECT gp_segment_id, count(*) FROM gs GROUP BY 1;")
	case "$out" in
		*"segment_of"*) notok "EXPLAIN of gp_segment_id" "$out" ;;
		*"Output: gp_segment_id, count(*)"*"Group Key: gs.gp_segment_id"*)
			ok "EXPLAIN prints it as the column" ;;
		*) notok "EXPLAIN of gp_segment_id" "$out" ;;
	esac
	q 0 "DELETE FROM gs WHERE gp_segment_id = 0;" >/dev/null
	q 0 "UPDATE gs SET b = gp_segment_id;" >/dev/null
	out=$(q 0 "SELECT count(*), count(*) FILTER (WHERE b <> expected_seg(a, 2)) FROM gs;")
	[ "$out" = "$n2|0" ] && ok "DELETE ... WHERE gp_segment_id = 0 empties segment 0; UPDATE sets it where each row is" \
		|| notok "DELETE and UPDATE with gp_segment_id" "$out (segment 1 held $n2)"
	out=$(q 1 "SELECT DISTINCT gp_segment_id FROM gre;")
	q 0 "CREATE TABLE gown (gp_segment_id int, a int) DISTRIBUTED BY (a); INSERT INTO gown VALUES (42, 1);" >/dev/null
	out2=$(q 0 "SELECT gp_segment_id FROM gown;")
	[ "$out|$out2" = "0|42" ] && ok "a segment's utility session answers its own id; a column of that name is the column" \
		|| notok "gp_segment_id on a segment, and a real column" "$out / $out2"

	###########################################################################
	echo "9. ANALYZE samples the segments, and the planner believes it"
	###########################################################################
	# O3: the coordinator's copy of a distributed table is empty, so ANALYZE
	# asks every segment for a sample and for its count, and pg_class and
	# pg_statistic on the coordinator describe the rows where they are.
	q 0 "CREATE TABLE st (a int, g int, v text) DISTRIBUTED BY (a);" >/dev/null
	q 0 "INSERT INTO st SELECT i, i % 10, CASE WHEN i % 4 = 0 THEN NULL ELSE md5(i::text) END FROM generate_series(1, 2000) i;" >/dev/null
	q 0 "ANALYZE st;" >/dev/null

	out=$(q 0 "SELECT reltuples FROM pg_class WHERE relname = 'st';")
	[ "$out" = "2000" ] && ok "reltuples on the coordinator counts the rows on every segment" \
		|| notok "reltuples after ANALYZE" "$out"

	pages=$(q 0 "SELECT relpages FROM pg_class WHERE relname = 'st';")
	seg=0
	for n in 1 2; do
		p=$(q "$n" "SELECT pg_relation_size('st') / current_setting('block_size')::int;")
		isnum "$p" && seg=$((seg + p))
	done
	[ "$pages" = "$seg" ] && ok "relpages is the segments' pages together ($pages)" \
		|| notok "relpages after ANALYZE" "$pages, segments: $seg"

	out=$(q 0 "SELECT n_distinct, null_frac FROM pg_stats WHERE tablename = 'st' AND attname IN ('g', 'v') ORDER BY attname;" | tr '\n' ' ')
	[ "$out" = "10|0 -0.75|0.25 " ] \
		&& ok "pg_stats is computed from the segments' rows" \
		|| notok "pg_stats after ANALYZE" "$out"

	out=$(q 0 "EXPLAIN SELECT * FROM st WHERE g = 3;" | sed -n 's/.*rows=\([0-9]*\).*/\1/p' | head -1)
	isnum "$out" && [ "$out" -ge 150 ] && [ "$out" -le 250 ] \
		&& ok "the planner estimates from those statistics, not the empty copy ($out rows)" \
		|| notok "the estimate after ANALYZE" "$out"

	q 0 "CREATE TABLE rst (a int) DISTRIBUTED REPLICATED;" >/dev/null
	q 0 "INSERT INTO rst SELECT generate_series(1, 300); ANALYZE rst;" >/dev/null
	out=$(q 0 "SELECT reltuples FROM pg_class WHERE relname = 'rst';")
	[ "$out" = "300" ] && ok "a replicated table's rows are counted once, not once a segment" \
		|| notok "reltuples of a replicated table" "$out"

	q 0 "ANALYZE sales;" >/dev/null
	out=$(q 0 "SELECT reltuples FROM pg_class WHERE relname = 'sales';")
	out2=$(q 0 "SELECT count(*) FROM pg_stats WHERE tablename = 'sales' AND inherited;")
	[ "$out" = "90" ] && isnum "$out2" && [ "$out2" -eq 3 ] \
		&& ok "a partitioned table is analyzed through its partitions" \
		|| notok "ANALYZE of a partitioned table" "$out / inherited stats: $out2"

	# On a segment, a utility session analyzes the rows it has, as vanilla does.
	own=$(q 1 "SELECT count(*) FROM st;")
	q 1 "ANALYZE st;" >/dev/null
	out=$(q 1 "SELECT reltuples FROM pg_class WHERE relname = 'st';")
	[ "$out" = "$own" ] && ok "ANALYZE in a utility session on a segment counts that segment's rows" \
		|| notok "ANALYZE on a segment" "$out, has $own"

	q 0 "CREATE ROLE analyze_nobody LOGIN;" >/dev/null
	out=$(q 0 "SET ROLE analyze_nobody; SELECT count(*) FROM gp_internal.sample_rows(NULL::st, 5);")
	case "$out" in
		*"permission denied"*) ok "a sample is refused to a role that cannot read the table" ;;
		*) notok "gp_internal.sample_rows without SELECT" "$out" ;;
	esac

	###########################################################################
	echo "10. ORCA's plans run on the segments, with Cloudberry's Motions"
	###########################################################################
	# ORCA's distributed layer: a Gather Motion's fragment is sent to the
	# segments as a plan of their own, only from a coordinator that has the
	# cluster secret, and the Motions between segments stream from slice to
	# slice, each slice on a backend of its own (gp_motion.c, gp_ic.c,
	# gp_share.c) -- or, with gp.interconnect_type = relay, are relayed
	# through the coordinator before it.
	SECRET="cluster-secret-$RANDOM$RANDOM$RANDOM"
	orca_started=1
	for n in 1 2 0; do
		start_node "$n" "shared_preload_libraries = '$PRELOAD,gp_orca'" \
			"gp.cluster_secret = '$SECRET'" || orca_started=0
	done
	out=$(q 0 "CREATE EXTENSION gp_orca;")
	if [ "$orca_started" -eq 1 ] && [ -z "$out" ]; then
		ok "the cluster restarts with ORCA and a secret"
	else
		notok "the cluster with ORCA" "$out $(tail -3 "$ROOT/node0.log")"
	fi

	q 0 "CREATE TABLE o (a int, b int, c text) DISTRIBUTED BY (a);" >/dev/null
	q 0 "INSERT INTO o SELECT i, i % 10, 'v' || i FROM generate_series(1, 1000) i;" >/dev/null
	q 0 "CREATE TABLE ro (b int, name text) DISTRIBUTED REPLICATED;" >/dev/null
	q 0 "INSERT INTO ro SELECT i, 'name' || i FROM generate_series(0, 9) i;" >/dev/null
	q 0 "CREATE TABLE po (x int, y int) DISTRIBUTED BY (x);" >/dev/null
	q 0 "INSERT INTO po SELECT i, i % 7 FROM generate_series(1, 500) i;" >/dev/null
	q 0 "CREATE TABLE bo (k int, s text) DISTRIBUTED BY (k);" >/dev/null
	q 0 "INSERT INTO bo SELECT i, md5((i % 20000)::text) FROM generate_series(1, 60000) i;" >/dev/null
	q 0 "ANALYZE o; ANALYZE ro; ANALYZE po; ANALYZE bo;" >/dev/null

	# ORCA planned it, with a Gather Motion, and the rows are the planner's.
	orca_same() {				# orca_same <what> <sql> [what EXPLAIN must say]
		local plan want got
		plan=$(q 0 "EXPLAIN (COSTS OFF) $2")
		want=$(q 0 "SET gp.optimizer = off; $2")
		got=$(q 0 "$2")
		case "$plan" in
			*"Optimizer: GPORCA"*) ;;
			*) notok "$1: planned by ORCA" "$plan"; return ;;
		esac
		if [ -n "${3:-}" ] && [[ "$plan" != *"$3"* ]]; then
			notok "$1: EXPLAIN says \"$3\"" "$plan"; return
		fi
		[ "$got" = "$want" ] && [ -n "$got" ] && ok "$1" \
			|| notok "$1: the same rows as the planner's" "ORCA: $got / planner: $want"
	}

	orca_same "a filtered scan, gathered" \
		"SELECT count(*), sum(a) FROM o WHERE b = 3;" \
		"Gather Motion 2:1  (slice1; segments: 2)"
	orca_same "an aggregate: partial on the segments, finished here" \
		"SELECT count(*), sum(a), max(c) FROM o;" "Partial Aggregate"
	orca_same "ORDER BY ... LIMIT: the segments' sorted rows, merged" \
		"SELECT a, c FROM o ORDER BY a LIMIT 5;" "Merge Key: o.a"
	orca_same "one key's rows: direct dispatch to its segment" \
		"SELECT * FROM o WHERE a = 42;" "Gather Motion 1:1  (slice1; segments: 1)"
	orca_same "a join of tables distributed alike, on the segments" \
		"SELECT count(*) FROM o o1 JOIN o o2 USING (a) WHERE o2.b = 1;"
	orca_same "a replicated table, read from one segment" \
		"SELECT count(*), max(name) FROM ro;" "Gather Motion 1:1"
	orca_same "a correlated subquery, run on the segments" \
		"SELECT a, (SELECT name FROM ro WHERE ro.b = o.b) FROM o WHERE a < 4 ORDER BY a;" \
		"SubPlan"
	orca_same "two gathers, one slice each" \
		"SELECT a FROM o WHERE a IN (SELECT b FROM o WHERE a < 20) ORDER BY a;" \
		"(slice2; segments: 2)"

	# ORCA takes no whole row, so a query naming gp_segment_id is PostgreSQL's.
	want=$(q 0 "SET gp.optimizer = off; SELECT gp_segment_id, count(*) FROM o GROUP BY 1 ORDER BY 1;")
	got=$(q 0 "SELECT gp_segment_id, count(*) FROM o GROUP BY 1 ORDER BY 1;")
	[ "$got" = "$want" ] && [ -n "$got" ] && ok "gp_segment_id under ORCA: planned by PostgreSQL, the same rows" \
		|| notok "gp_segment_id under ORCA" "$got / $want"

	# The slice table, in PlannedStmt.extension_state, as Cloudberry's
	# EXPLAIN (SLICETABLE) would print it.
	out=$(q 0 "SELECT string_agg(concat_ws(',', slice, parent, gang, segments, direct_segment), ' ' ORDER BY slice) FROM gp_orca.slices('SELECT y, count(*) FROM o JOIN po ON o.b = po.y GROUP BY y');")
	out2=$(q 0 "SELECT string_agg(concat_ws(',', slice, gang, direct_segment), ' ' ORDER BY slice) FROM gp_orca.slices('SELECT * FROM o WHERE a = 42');")
	[ "$out" = "0,unallocated,1 1,0,primary reader,2 2,1,primary reader,2 3,1,primary reader,2" ] && \
		[[ "$out2" == "0,unallocated 1,primary reader,"[01] ]] \
		&& ok "the slice table: each slice, the one it sends to, its gang, and direct dispatch's segment" \
		|| notok "the slice table" "$out / $out2"

	out=$(q 0 "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM o;")
	case "$out" in
		*"Gather Motion 2:1"*"(actual rows=2"*"Seq Scan on o (never executed)"*)
			ok "EXPLAIN ANALYZE: the segments' part is theirs, not run here" ;;
		*) notok "EXPLAIN ANALYZE of a Motion" "$out" ;;
	esac

	# The Motions between segments.
	orca_same "a GROUP BY off the key: partial aggregates, redistributed by it" \
		"SELECT b, count(*), avg(a) FROM o GROUP BY b ORDER BY b;" \
		"Redistribute Motion 2:2  (slice2; segments: 2)"
	orca_same "a join on columns neither table is distributed by" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y;" "Hash Key: po.y"
	orca_same "a small side broadcast to every segment" \
		"SELECT count(*) FROM o JOIN (SELECT * FROM po WHERE x < 10) s ON o.b = s.y;" \
		"Broadcast Motion 2:2"
	orca_same "rows every segment has, each kept where it hashes" \
		"SELECT count(*) FROM o JOIN generate_series(1, 100) g ON o.b = g;" \
		"Hash Filter"
	orca_same "a join redistributed, then aggregated: two Motions below a Gather" \
		"SELECT y, count(*) FROM o JOIN po ON o.b = po.y GROUP BY y ORDER BY y;" \
		"(slice3; segments: 2)"
	orca_same "sixty thousand rows relayed in many batches" \
		"SELECT count(*), sum(length(s)) FROM (SELECT s, count(*) FROM bo GROUP BY s) x;" \
		"Redistribute Motion"
	orca_same "two Gathers, each with a Broadcast below it, each dropping its rows after" \
		"SELECT count(*) FROM (SELECT o.a, o.b FROM o JOIN (SELECT * FROM po WHERE x < 20) s ON o.b = s.y) l JOIN (SELECT o.a, o.b FROM o JOIN (SELECT * FROM po WHERE x < 30) s2 ON o.b = s2.y) r ON l.a = r.b;" \
		"(slice4; segments: 2)"
	orca_same "a window partitioned off the key, merged in order" \
		"SELECT a, rank() OVER (PARTITION BY b ORDER BY a DESC) FROM o WHERE a > 990 ORDER BY b, a;" \
		"Merge Key"

	# The slices of a query run at once: the writer on each segment runs the
	# Gather's, and readers -- more backends of the session there, reading as
	# a part of the writer's transaction -- run the others, streaming their
	# rows to the slice above.
	relay_same() {				# relay_same <what> <sql>: tcp and relay agree
		local tcp relay
		tcp=$(q 0 "$2")
		relay=$(q 0 "SET gp.interconnect_type = relay; $2")
		[ "$tcp" = "$relay" ] && [ -n "$tcp" ] && ok "$1" \
			|| notok "$1: streamed and relayed rows agree" "tcp: $tcp / relay: $relay"
	}
	relay_same "streamed and relayed, the same rows: two Motions below a Gather" \
		"SELECT y, count(*), sum(a) FROM o JOIN po ON o.b = po.y GROUP BY y ORDER BY y;"
	relay_same "streamed and relayed, the same rows: sixty thousand rows between segments" \
		"SELECT count(*), sum(length(s)) FROM (SELECT s, count(*) FROM bo GROUP BY s) x;"

	# The readers, seen from a segment while the session that used them lives:
	# as many on each segment as the widest statement had slices below its
	# Gather, kept for the next statement rather than started again.
	{
		for i in 1 2 3 4 5 6 7 8; do
			echo "SELECT count(*) FROM o JOIN po ON o.b = po.y;"
		done
		echo "SELECT pg_sleep(4);"
	} | qf 0 > "$ROOT/readers.out" 2>&1 &
	bg=$!
	sleep 2.5
	readers=$(q 1 "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'cloudberry reader';")
	sockets=$(ls -a "$(sockdir 1)" | grep -c '^\.s\.GPIC\.')
	wait $bg
	[ "$readers" = "2" ] && [ "$sockets" -ge 3 ] \
		&& ok "each slice below the Gather ran on a reader of its own, kept for the session ($readers readers, $sockets interconnect sockets on segment 0)" \
		|| notok "the readers on a segment" "readers $readers, sockets $sockets"

	# A reader sees what the writer's transaction wrote before the statement:
	# rows, a table made in it (R4: the catalog read as of the writer's
	# command), a row updated in it (a combo command ID the reader looks up in
	# what the writer published, R2), and it takes no lock the writer's
	# TRUNCATE would make it wait for (the writer's lock group).
	out=$(printf '%s\n' "BEGIN;" \
		"INSERT INTO po SELECT i, 77 FROM generate_series(1001, 1010) i;" \
		"SELECT count(*) FROM o JOIN po ON o.b + 70 = po.y;" "ROLLBACK;" | qf 0)
	[ "$out" = "1000" ] && ok "a reader reads the rows the writer's transaction has not committed" \
		|| notok "uncommitted rows, read by a reader" "$out"
	out=$(printf '%s\n' "SET client_min_messages = warning;" "BEGIN;" \
		"CREATE TABLE sn (x int, y int) DISTRIBUTED BY (x);" \
		"INSERT INTO sn SELECT i, i % 7 FROM generate_series(1, 70) i;" "ANALYZE sn;" \
		"SELECT count(*) FROM o JOIN sn ON o.b = sn.y;" \
		"ALTER TABLE sn ADD COLUMN z int DEFAULT 2;" \
		"SELECT sum(z) FROM o JOIN sn ON o.b = sn.y;" "ROLLBACK;" | qf 0)
	[ "$out" = "7000
14000" ] && ok "and a table made and altered in it, which its catalog shows the reader" \
		|| notok "a table made in the transaction, read by a reader" "$out"
	out=$(printf '%s\n' "BEGIN;" \
		"INSERT INTO po SELECT i, 88 FROM generate_series(2001, 2010) i;" \
		"UPDATE po SET y = 89 WHERE x > 2005;" \
		"SELECT po.y, count(*) FROM o JOIN po ON o.b + 80 = po.y GROUP BY po.y ORDER BY 1;" \
		"ROLLBACK;" | qf 0)
	[ "$out" = "88|500
89|500" ] && ok "and rows it inserted and then updated, whose combo command IDs the writer publishes" \
		|| notok "combo command IDs, resolved by a reader" "$out"
	q 0 "CREATE TABLE sttr (x int, y int) DISTRIBUTED BY (x);" >/dev/null
	out=$(printf '%s\n' "SET lock_timeout = '10s';" "BEGIN;" "TRUNCATE sttr;" \
		"INSERT INTO sttr SELECT i, i % 3 FROM generate_series(1, 30) i;" \
		"SELECT count(*) FROM sttr JOIN o ON sttr.y = o.b;" "COMMIT;" | qf 0)
	[ "$out" = "3000" ] && ok "a reader does not wait for the lock of the writer's TRUNCATE" \
		|| notok "a TRUNCATE in the transaction, and a reader" "$out"
	out=$(printf '%s\n' "BEGIN;" "INSERT INTO po VALUES (3001, 99);" "SAVEPOINT s;" \
		"INSERT INTO po VALUES (3002, 99);" \
		"SELECT count(*) FROM o JOIN po ON o.b + 90 = po.y;" "ROLLBACK TO s;" \
		"SELECT count(*) FROM o JOIN po ON o.b + 90 = po.y;" "ROLLBACK;" | qf 0)
	[ "$out" = "200
100" ] && ok "a savepoint rolled back is rolled back for the readers of the next statement" \
		|| notok "a savepoint and the readers" "$out"

	# A slice that fails fails the statement with its own error, not with the
	# interconnect's word for a sender that stopped; a LIMIT stops the
	# senders; a cursor's slices wait for its next FETCH.
	out=$(q 0 "SELECT count(*) FROM o JOIN po ON o.b = po.y WHERE 1 / (po.x - 250) > -1;")
	case "$out" in
		*"division by zero"*) ok "an error in a sending slice is the statement's error" ;;
		*) notok "an error in a sending slice" "$out" ;;
	esac
	out=$(printf '%s\n' "SELECT count(*) FROM (SELECT bo.k FROM bo JOIN po ON bo.k % 7 = po.y LIMIT 3) l;" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y;" | qf 0)
	want=$(q 0 "SET gp.interconnect_type = relay; SELECT count(*) FROM o JOIN po ON o.b = po.y;")
	[ "$out" = "3
$want" ] && ok "a LIMIT stops its senders, and the session goes on" \
		|| notok "a LIMIT above streaming slices" "$out / $want"
	cursor="BEGIN;
DECLARE c CURSOR FOR SELECT po.y, count(*) FROM o JOIN po ON o.b = po.y GROUP BY po.y ORDER BY 1;
FETCH 2 FROM c;
SELECT count(*) FROM o JOIN po ON o.b = po.y;
FETCH 2 FROM c;
COMMIT;"
	out=$(echo "$cursor" | qf 0)
	want=$(printf '%s\n' "SET gp.interconnect_type = relay;" "$cursor" | qf 0)
	[ "$out" = "$want" ] && [ -n "$out" ] \
		&& ok "a cursor's slices wait for its next FETCH while another statement streams" \
		|| notok "a cursor and another statement, streaming" "$out / $want"

	# A temporary table is read only by its session's own backend.
	out=$(printf '%s\n' "CREATE TEMP TABLE tmpo (x int, y int) DISTRIBUTED BY (x);" \
		"INSERT INTO tmpo SELECT i, i % 7 FROM generate_series(1, 70) i;" \
		"SELECT count(*) FROM o JOIN tmpo ON o.b = tmpo.y;" | qf 0)
	[ "$out" = "7000" ] && ok "a temporary table: its Motions are relayed" \
		|| notok "a temporary table and streaming" "$out"

	# ORCA's writes, carried out where the rows are.
	placed() {					# placed <table>: rows on the wrong segment
		local w1 w2
		w1=$(q 1 "SELECT count(*) FROM $1 WHERE expected_seg(a, 2) <> 0;")
		w2=$(q 2 "SELECT count(*) FROM $1 WHERE expected_seg(a, 2) <> 1;")
		echo "$w1|$w2"
	}
	q 0 "CREATE TABLE wo (a int, b int, c text) DISTRIBUTED BY (a);" >/dev/null
	q 0 "CREATE INDEX wo_b ON wo (b);" >/dev/null
	plan=$(q 0 "EXPLAIN (COSTS OFF) INSERT INTO wo SELECT y, x, 'p' || x FROM po;")
	tag=$("$PSQL" -X -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
		-c "INSERT INTO wo SELECT y, x, 'p' || x FROM po;" 2>&1)
	out=$(q 0 "SELECT count(*), sum(a), sum(b) FROM wo;")
	case "$plan" in
		*"Dispatch  (slice1; segments: 2)"*"Insert on wo"*"Redistribute Motion 2:2"*"Optimizer: GPORCA"*)
			[ "$tag|$out|$(placed wo)" = "INSERT 0 500|500|1497|125250|0|0" ] \
				&& ok "INSERT ... SELECT: redistributed by the target's key, written on the segments" \
				|| notok "ORCA's INSERT ... SELECT" "$tag / $out / misplaced $(placed wo)" ;;
		*) notok "ORCA's INSERT ... SELECT: the plan" "$plan" ;;
	esac

	tag=$("$PSQL" -X -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
		-c "UPDATE wo SET c = 'u' WHERE a = 3;" 2>&1)
	out=$(q 0 "SELECT count(*) FROM wo WHERE c = 'u';")
	[ "$tag|$out" = "UPDATE 72|72" ] && ok "an UPDATE on the segments, counted ($tag)" \
		|| notok "ORCA's UPDATE" "$tag / $out"

	tag=$("$PSQL" -X -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
		-c "DELETE FROM o WHERE a = 5;" 2>&1)
	out=$(q 0 "SELECT count(*) FROM o;")
	[ "$tag|$out" = "DELETE 1|999" ] && ok "a DELETE on the segments ($tag)" \
		|| notok "ORCA's DELETE" "$tag / $out"

	# Split: the key changes, and each row moves to the segment it hashes to.
	plan=$(q 0 "EXPLAIN (COSTS OFF) UPDATE wo SET a = a + 1000 WHERE b < 50;")
	tag=$("$PSQL" -X -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
		-c "UPDATE wo SET a = a + 1000 WHERE b < 50;" 2>&1)
	out=$(q 0 "SELECT count(*), sum(a), count(*) FILTER (WHERE a >= 1000) FROM wo;")
	idx=$(q 0 "SET enable_seqscan = off; SELECT count(*) FROM wo WHERE b = 7;")
	case "$plan" in
		*"Update on wo"*"Redistribute Motion 2:2"*"Split Update"*)
			[ "$tag|$out|$idx|$(placed wo)" = "UPDATE 49|500|50497|49|1|0|0" ] \
				&& ok "an UPDATE of the key: Split, each row moved where it hashes, its index entries with it" \
				|| notok "a split update" "$tag / $out / index $idx / misplaced $(placed wo)" ;;
		*) notok "a split update: the plan" "$plan" ;;
	esac

	out=$(printf '%s\n' "BEGIN;" "UPDATE wo SET a = -a;" "ROLLBACK;" \
		"SELECT count(*), sum(a) FROM wo;" | qf 0)
	[ "$out" = "500|50497" ] && ok "a split update rolled back leaves every row where it was" \
		|| notok "a split update rolled back" "$out"

	q 0 "CREATE TABLE wt (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	q 0 "CREATE FUNCTION wt_noop() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RETURN NEW; END \$\$;" >/dev/null
	q 0 "CREATE TRIGGER wt_t BEFORE INSERT ON wt FOR EACH ROW EXECUTE FUNCTION wt_noop();" >/dev/null
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"UPDATE wt SET a = a + 1;" | qf 0)
	case "$out" in
		*"on a table with triggers"*) ok "an UPDATE of the key of a table with triggers is refused, and says why" ;;
		*) notok "a split update with triggers" "$out" ;;
	esac

	# A slice's parameters travel with it, as Cloudberry's dispatcher sends
	# them: the statement's own, and the values the coordinator computes.
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"SET plan_cache_mode = force_generic_plan;" \
		"PREPARE p(int) AS SELECT count(*) FROM o WHERE b = \$1;" "EXECUTE p(3);" \
		"EXECUTE p(4);" "EXPLAIN (COSTS OFF) EXECUTE p(3);" | qf 0 | tr '\n' ' ')
	case "$out" in
		*"fallback"*|*"Feature not supported"*) notok "a generic plan's parameter" "$out" ;;
		"100 100 "*"Gather Motion"*) ok "a generic plan's parameter is sent with the fragment, and ORCA plans it" ;;
		*) notok "a parameter in a fragment" "$out" ;;
	esac

	# A sequence is the coordinator's: a plan that would take its next value
	# on a segment -- a random table's row made there -- is the planner's.
	q 0 "CREATE TABLE sr (n serial, v int) DISTRIBUTED RANDOMLY;" >/dev/null
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"INSERT INTO sr (v) VALUES (1), (2);" "INSERT INTO sr (v) VALUES (3);" \
		"SELECT count(DISTINCT n), min(n), max(n) FROM sr;" | qf 0 | tail -1)
	[ "$out" = "3|1|3" ] && ok "a sequence's next value is taken on the coordinator, never a segment" \
		|| notok "a serial column of a random table under ORCA" "$out"

	q 0 "CREATE FUNCTION count_sql(int) RETURNS bigint LANGUAGE sql AS 'SELECT count(*) FROM o WHERE b = \$1';" >/dev/null
	out=$(q 0 "SELECT count_sql(3), count_sql(4), count_sql(NULL);")
	case "$out" in
		"100|100|0") ok "a SQL function's argument is sent as the statement parameter it is" ;;
		*) notok "a SQL function's argument in a fragment" "$out" ;;
	esac

	q 0 "CREATE FUNCTION count_b(x int) RETURNS bigint LANGUAGE plpgsql AS \$\$ BEGIN RETURN (SELECT count(*) FROM o WHERE b = x); END \$\$;" >/dev/null
	out=$(q 0 "SELECT count_b(3), count_b(4), count_b(NULL);")
	case "$out" in
		"100|100|0") ok "a PL/pgSQL variable is sent as the statement parameter it is" ;;
		*) notok "a PL/pgSQL variable in a fragment" "$out" ;;
	esac

	q 0 "CREATE FUNCTION count_o() RETURNS bigint LANGUAGE plpgsql AS \$\$ BEGIN RETURN (SELECT count(*) FROM o); END \$\$;" >/dev/null
	out=$(q 0 "SELECT a, count_o() FROM o WHERE a < 3;")
	case "$out" in
		*"function cannot execute on a QE slice because it accesses relation \"public.o\""*)
			ok "a function in a fragment may not read a segment's share as the table" ;;
		*) notok "a function reading a table on a segment" "$out" ;;
	esac

	# A segment knows each table's distribution: the coordinator sends it
	# every "gp" label it changes, in the statement's transaction.
	q 0 "CREATE TABLE lp (a int, b int) DISTRIBUTED BY (b);" >/dev/null
	q 0 "CREATE TABLE lr (k int, v text) DISTRIBUTED REPLICATED;" >/dev/null
	q 0 "INSERT INTO lr SELECT i, 'r' || i FROM generate_series(1, 10) i;" >/dev/null
	out=""
	for n in 0 1 2; do
		out="$out$(q "$n" "SELECT gp_sql.distribution('lp') || ' ' || gp_sql.distribution('lr');")/"
	done
	[ "$out" = "(b) replicated/(b) replicated/(b) replicated/" ] \
		&& ok "every segment has the coordinator's policy of a table CREATE TABLE made" \
		|| notok "a table's policy on the segments" "$out"

	q 0 "ALTER TABLE lp SET DISTRIBUTED BY (a);" >/dev/null
	out=$(printf '%s\n' "BEGIN;" "ALTER TABLE lp SET DISTRIBUTED RANDOMLY;" "ROLLBACK;" \
		"BEGIN;" "SAVEPOINT s;" "ALTER TABLE lp SET DISTRIBUTED REPLICATED;" \
		"ROLLBACK TO SAVEPOINT s;" "COMMIT;" | qf 0 >/dev/null; \
		for n in 1 2; do q "$n" "SELECT gp_sql.distribution('lp');"; done | tr '\n' ' ')
	[ "$out" = "(a) (a) " ] \
		&& ok "ALTER TABLE ... SET DISTRIBUTED reaches the segments, and a rolled-back one does not" \
		|| notok "a changed policy on the segments" "$out"

	q 0 "CREATE FUNCTION lr_v(int) RETURNS text LANGUAGE sql STABLE AS 'SELECT v FROM lr WHERE k = \$1';" >/dev/null
	n=$(q 0 "SELECT count(*) FROM o;")
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"SELECT count(*), count(lr_v(a % 10 + 1)) FROM o;" | qf 0)
	case "$out" in
		*"fallback"*|*"QE slice"*) notok "a function in a fragment reading a replicated table" "$out" ;;
		"$n|$n") ok "a function in a fragment may read a replicated table, which every segment holds whole" ;;
		*) notok "a function in a fragment reading a replicated table" "$out" ;;
	esac

	# A segment takes a plan only from a connection with the secret.
	frag="SELECT gp_internal.exec_fragment('{PLANNEDSTMT :commandType 1}', '');"
	for opts in "-c gp.qe_identity=seg0/dbid1/sess1" \
		"-c gp.qe_identity=seg0/dbid1/sess1 -c gp.qe_secret=not-the-secret-at-all"; do
		out=$(PGOPTIONS="$opts" q 1 "$frag")
		case "$out" in
			*"a plan is carried out only for the coordinator"*) ok "a segment refuses a plan: $opts" ;;
			*) notok "a plan from a client that says it is the dispatcher" "$out" ;;
		esac
	done
	out=$(PGOPTIONS="-c gp.qe_identity=seg0/dbid1/sess1" \
		q 1 "SELECT gp_internal.motion_put('k', 2, '\\x00'::bytea);")
	case "$out" in
		*"a Motion's rows are taken only from the coordinator"*) ok "a segment takes a Motion's rows only from the coordinator" ;;
		*) notok "motion_put from a client that says it is the dispatcher" "$out" ;;
	esac
	out=$(q 0 "CREATE ROLE secret_reader LOGIN;" >/dev/null; q 0 "SET ROLE secret_reader; SHOW gp.cluster_secret;")
	case "$out" in
		*"permission denied"*) ok "the secret cannot be read by an ordinary role" ;;
		*) notok "SHOW gp.cluster_secret" "$out" ;;
	esac

	# No secret on the coordinator: ORCA is told, and the planner gathers.
	start_node 0 "shared_preload_libraries = '$PRELOAD,gp_orca'"
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"SELECT count(*), sum(a) FROM o WHERE b = 3;" | qf 0)
	case "$out" in
		*"a Motion, without gp.cluster_secret"*"100|49800") ok "without a secret nothing is dispatched as a plan, and the answer is the same" ;;
		*) notok "ORCA without a secret" "$out" ;;
	esac

	###########################################################################
	echo "11. Cloudberry's settings of the dispatcher and the planner"
	###########################################################################
	# gp_settings.c: the ones the port carries out, and the ones it accepts
	# for Cloudberry's scripts with nothing to apply them to yet.
	q 0 "CREATE TABLE ds (key int, v text) DISTRIBUTED BY (key);" >/dev/null
	out=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" \
		"SET gp.optimizer = off;" \
		"INSERT INTO ds VALUES (100, 'cow');" \
		"INSERT INTO ds VALUES (1, 'a'), (2, 'b');" \
		"SELECT count(*) FROM ds;" \
		"SELECT v FROM ds WHERE key = 100;" \
		"UPDATE ds SET v = 'horse' WHERE key = 100;" \
		"DELETE FROM ds WHERE key = 1;" | qf 0 | grep -o 'INFO:.*' | tr '\n' '/')
	expect="INFO:  (slice 0) Dispatch command to SINGLE content/INFO:  (slice 0) Dispatch command to ALL contents: 0 1/INFO:  (slice 1) Dispatch command to ALL contents: 0 1/INFO:  (slice 1) Dispatch command to SINGLE content/INFO:  (slice 0) Dispatch command to SINGLE content/INFO:  (slice 0) Dispatch command to SINGLE content/"
	[ "$out" = "$expect" ] \
		&& ok "the planner's dispatches print Cloudberry's INFO lines, one segment where the key names one" \
		|| notok "gp.test_print_direct_dispatch_info under the planner" "$out"

	out=$(q 0 "SELECT key, v FROM ds ORDER BY key;" | tr '\n' ' ')
	[ "$out" = "2|b 100|horse " ] && ok "an UPDATE and a DELETE sent to one segment change the rows the key names" \
		|| notok "the rows after direct dispatch" "$out"

	out=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" \
		"INSERT INTO ds VALUES (7, 'x');" \
		"SELECT count(*) FROM ds;" \
		"SELECT v FROM ds WHERE key = 7;" \
		"DELETE FROM ds WHERE key = 7;" | qf 0 | grep -o 'INFO:.*' | tr '\n' '/')
	expect="INFO:  (slice 0) Dispatch command to SINGLE content/INFO:  (slice 1) Dispatch command to ALL contents: 0 1/INFO:  (slice 1) Dispatch command to SINGLE content/INFO:  (slice 0) Dispatch command to SINGLE content/"
	[ "$out" = "$expect" ] \
		&& ok "ORCA's slices print theirs, a write as slice 0 and one row of constants to its segment" \
		|| notok "gp.test_print_direct_dispatch_info under ORCA" "$out"

	out=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" \
		"SET gp.enable_direct_dispatch = off;" \
		"SELECT v FROM ds WHERE key = 100;" \
		"SET gp.optimizer = off;" \
		"SELECT v FROM ds WHERE key = 100;" | qf 0 | grep -o 'INFO:.*' | tr '\n' '/')
	expect="INFO:  (slice 1) Dispatch command to ALL contents: 0 1/INFO:  (slice 1) Dispatch command to ALL contents: 0 1/"
	[ "$out" = "$expect" ] && ok "gp.enable_direct_dispatch = off asks every segment, under either planner" \
		|| notok "gp.enable_direct_dispatch = off" "$out"

	out=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" \
		"EXPLAIN SELECT v FROM ds WHERE key = 100;" | qf 0 | grep -c 'INFO:')
	[ "$out" = "0" ] && ok "EXPLAIN dispatches nothing, and says so by printing nothing" \
		|| notok "INFO lines under EXPLAIN" "$out"

	# Autostats, as Cloudberry's auto_stats() decides.  Its default is none.
	out=$(printf '%s\n' "CREATE TABLE as1 (a int) DISTRIBUTED BY (a);" \
		"INSERT INTO as1 SELECT generate_series(1, 1000);" \
		"SELECT reltuples FROM pg_class WHERE relname = 'as1';" \
		"SET gp.autostats_mode = on_no_stats;" \
		"CREATE TABLE as2 (a int) DISTRIBUTED BY (a);" \
		"INSERT INTO as2 SELECT generate_series(1, 1000);" \
		"SELECT reltuples FROM pg_class WHERE relname = 'as2';" \
		"INSERT INTO as2 SELECT generate_series(1, 500);" \
		"SELECT reltuples FROM pg_class WHERE relname = 'as2';" \
		"CREATE TABLE as3 AS SELECT generate_series(1, 300) a DISTRIBUTED BY (a);" \
		"SELECT reltuples FROM pg_class WHERE relname = 'as3';" \
		"SET gp.autostats_mode = on_change;" \
		"SET gp.autostats_on_change_threshold = 400;" \
		"INSERT INTO as2 SELECT generate_series(1, 300);" \
		"SELECT reltuples FROM pg_class WHERE relname = 'as2';" \
		"DELETE FROM as2 WHERE a <= 450;" \
		"SELECT reltuples FROM pg_class WHERE relname = 'as2';" | qf 0 | tr '\n' ' ')
	[ "$out" = "-1 1000 1000 300 1000 600 " ] \
		&& ok "autostats: none by default; on_no_stats after the first INSERT and a CTAS; on_change past the threshold" \
		|| notok "gp.autostats_mode" "$out"

	out=$(printf '%s\n' "SET gp.optimizer = off;" "SET enable_hashagg = off;" \
		"EXPLAIN (COSTS OFF) SELECT v, count(*) FROM ds GROUP BY v;" \
		"RESET enable_hashagg;" "SET gp.enable_groupagg = off;" \
		"EXPLAIN (COSTS OFF) SELECT v, count(*) FROM ds GROUP BY v;" | qf 0 | grep -o '^ *[A-Za-z]*Aggregate' | tr -d ' ' | tr '\n' ' ')
	[ "$out" = "GroupAggregate HashAggregate " ] && ok "gp.enable_groupagg = off turns the planner from sorted grouping to hashed" \
		|| notok "gp.enable_groupagg" "$out"

	out=$(printf '%s\n' "SET gp.statement_mem = '2MB';" "SET gp.enable_parallel = on;" \
		"SET gp.interconnect_queue_depth = 8;" "SET gp.enable_multiphase_agg = off;" \
		"SET gp.motion_cost_per_row = 0.5;" "SHOW gp.statement_mem;" | qf 0)
	[ "$out" = "2MB" ] && ok "Cloudberry's other settings are accepted, and say what they do here" \
		|| notok "Cloudberry's accepted settings" "$out"

	###########################################################################
	echo "12. the segments authenticate the coordinator, with SCRAM"
	###########################################################################
	# Decision 5 asks for SCRAM on the early milestones.  The dispatcher is an
	# ordinary client, so this is ordinary authentication: the segment asks,
	# and the coordinator answers from the password file gp.internal_passfile
	# names -- a file, because a setting is readable by anyone who can SHOW it.
	for n in 1 2; do
		q "$n" "ALTER ROLE $(whoami) PASSWORD 'cluster-secret';" >/dev/null
		sed -i 's/^local *all *all *trust$/local all all scram-sha-256/' \
			"$(datadir "$n")/pg_hba.conf"
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -w -t 30 reload >/dev/null 2>&1
	done

	printf '*:*:*:*:cluster-secret\n' > "$ROOT/passfile"
	chmod 600 "$ROOT/passfile"

	out=$(q 0 "SELECT count(*) FROM gp.exec_on_segments('SELECT 1');")
	case "$out" in
		*"authentication failed"*|*"no password supplied"*)
			ok "without the password file the segments refuse the dispatcher" ;;
		*) notok "a segment should have asked for a password" "$out" ;;
	esac

	if start_node 0 "gp.internal_passfile = '$ROOT/passfile'"; then
		out=$(q 0 "SELECT count(*) FROM gp.exec_on_segments('SELECT 1');")
		[ "$out" = "2" ] && ok "with it, the dispatcher authenticates and both segments answer" \
			|| notok "dispatch with a password file" "$out"
	else
		notok "the coordinator starts with gp.internal_passfile" \
			"$(tail -3 "$ROOT/node0.log")"
	fi

	# Put the segments back to trust, so that what follows is about the
	# cluster configuration and not about passwords.
	for n in 1 2; do
		sed -i 's/^local all all scram-sha-256$/local   all   all   trust/' \
			"$(datadir "$n")/pg_hba.conf"
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -w -t 30 reload >/dev/null 2>&1
	done
fi

###############################################################################
echo "13. a cluster described wrongly is a server that does not start"
###############################################################################
# The message has to name the file and the line: this is read in the
# postmaster while it starts, so it is all the operator is given.
refuses() {					# refuses <what> <expected message> <config lines...>
	local what="$1"; local want="$2"; shift 2
	local d; d="$(datadir 0)"

	"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir 0)'"
		echo "listen_addresses = ''"
		echo "port = $(port 0)"
		for line in "$@"; do echo "$line"; done
	} > "$d/postgresql.auto.conf"

	: > "$ROOT/node0.log"
	if "$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node0.log" -w -t 20 start >/dev/null 2>&1; then
		notok "$what" "the server started"
		"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
		return
	fi
	if grep -qF "$want" "$ROOT/node0.log"; then
		ok "$what"
	else
		notok "$what" "$(grep -i 'FATAL\|ERROR' "$ROOT/node0.log" | head -2)"
	fi
}

cat > "$ROOT/bad-role.conf" <<EOF
1 -1 x $(sockdir 0) $(port 0) $(datadir 0)
EOF
refuses "a role that is neither p nor m names the line" \
	"line 1" \
	"gp.cluster_config = '$ROOT/bad-role.conf'" "gp.dbid = 1"

cat > "$ROOT/two-coords.conf" <<EOF
1 -1 p $(sockdir 0) $(port 0) $(datadir 0)
2 -1 p $(sockdir 1) $(port 1) $(datadir 1)
EOF
refuses "two lines for content -1 are refused" \
	"has two nodes with role" \
	"gp.cluster_config = '$ROOT/two-coords.conf'" "gp.dbid = 1"

cat > "$ROOT/hole.conf" <<EOF
1 -1 p $(sockdir 0) $(port 0) $(datadir 0)
2  1 p $(sockdir 1) $(port 1) $(datadir 1)
EOF
refuses "a hole in the content ids is refused" \
	"content id 1 is out of range" \
	"gp.cluster_config = '$ROOT/hole.conf'" "gp.dbid = 1"

refuses "a dbid that is in no line is refused" \
	"is not in cluster configuration file" \
	"gp.cluster_config = '$CONF'" "gp.dbid = 99"

refuses "a role that disagrees with the file is refused" \
	"gives dbid 2 content id 0" \
	"gp.cluster_config = '$CONF'" "gp.dbid = 2" "gp.role = 'dispatch'"

refuses "dispatch with no cluster at all is refused" \
	"no cluster is configured" \
	"gp.role = 'dispatch'"

refuses "a file that is not there is refused" \
	"could not open cluster configuration file" \
	"gp.cluster_config = '$ROOT/nowhere.conf'"

###############################################################################
echo "14. with no cluster configured, this is a single node"
###############################################################################
if start_node 0 "gp.cluster_config = ''" ; then
	notok "node 0 should not have started: gp.role is dispatch with no cluster"
else
	ok "gp.role = dispatch with no cluster is still refused"
fi

d="$(datadir 0)"
"$BINDIR/pg_ctl" -D "$d" -m immediate stop >/dev/null 2>&1
{
	echo "shared_preload_libraries = '$PRELOAD'"
	echo "unix_socket_directories = '$(sockdir 0)'"
	echo "listen_addresses = ''"
	echo "port = $(port 0)"
} > "$d/postgresql.auto.conf"
if "$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node0.log" -w -t 30 start >/dev/null 2>&1; then
	out=$(q 0 "SELECT role, segments, content_id, single_node, dbid FROM gp.node();")
	[ "$out" = "utility|1|-1|t|1" ] \
		&& ok "one node: utility, one segment to compute with, single_node ($out)" \
		|| notok "gp.node() with no cluster" "$out"

	out=$(q 0 "SELECT count(*) FROM gp.segment_configuration();")
	[ "$out" = "0" ] && ok "and no rows in gp.segment_configuration()" \
		|| notok "gp.segment_configuration() with no cluster" "$out"

	out=$(q 0 "CREATE TABLE sn (a int); INSERT INTO sn VALUES (1), (2); SELECT DISTINCT gp_segment_id FROM sn;")
	[ "$out" = "-1" ] && ok "gp_segment_id is -1, as on Cloudberry's single node" \
		|| notok "gp_segment_id on one node" "$out"
else
	notok "a server with no cluster starts" "$(tail -5 "$ROOT/node0.log")"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
