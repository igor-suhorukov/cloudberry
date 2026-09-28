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
	# node 3 is the segment section 18 adds to the running cluster
	for n in 0 1 2 3; do
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
		# a transaction that writes on a segment is prepared there (gp_dtx.c)
		echo "max_prepared_transactions = 16"
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

	# Cloudberry's catalogs of the cluster, by their names (gp_catalog.c).
	out=$(q 0 "SELECT dbid, content, role, preferred_role, mode, status, port FROM gp_segment_configuration ORDER BY dbid;")
	out2=$(q 1 "SELECT count(*) FROM gp_segment_configuration c JOIN gp.segment_configuration() s ON s.dbid = c.dbid AND s.hostname = c.hostname AND s.hostname = c.address AND s.datadir = c.datadir AND c.warehouseid = 0;")
	want="1|-1|p|p|n|u|$(port 0)
2|0|p|p|n|u|$(port 1)
3|1|p|p|n|u|$(port 2)"
	[ "$out" = "$want" ] && [ "$out2" = "3" ] \
		&& ok "gp_segment_configuration is the file, in Cloudberry's columns, every node up" \
		|| notok "gp_segment_configuration" "$out / $out2"

	out=$(q 0 "SELECT gpname, numsegments, dbid, content FROM gp_id;")
	out2=$(q 0 "SELECT string_agg(gp_segment_id || ':' || gpname, ' ' ORDER BY gp_segment_id) FROM gp.dist_random(NULL::gp_id) g;")
	[ "$out|$out2" = "Cloudberry|-1|-1|-1|0:Cloudberry 1:Cloudberry" ] \
		&& ok "gp_id has its one row on every node, so a query over it runs once on each segment" \
		|| notok "gp_id" "$out / $out2"

	out=$(q 0 "INSERT INTO gp_configuration_history VALUES (now(), 1, 'x');")
	out2=$(printf '%s\n' "BEGIN;" "SET LOCAL allow_system_table_mods = on;" \
		"INSERT INTO gp_configuration_history VALUES ('2026-09-23 10:00+00', 2, 'by hand');" \
		"SELECT dbid || ' ' || \"desc\" FROM gp_configuration_history;" "ROLLBACK;" \
		"SELECT count(*) FROM gp_configuration_history;" | qf 0 | tr '\n' '/')
	case "$out|$out2" in
		*'permission denied: "gp_configuration_history" is a system catalog'*"|2 by hand/0/")
			ok "gp_configuration_history is written only with allow_system_table_mods, as a catalog is" ;;
		*) notok "gp_configuration_history" "$out / $out2" ;;
	esac

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

	# The session id is the coordinator's number of the session, taken as the
	# client connects, as Cloudberry's gp_session_id is: three sessions opened
	# one after another have ids each past the last -- the next, but for a
	# process of the server's that dispatched in between -- and what a
	# session's backend on a segment says it works for is its session's.
	out=$(for i in 1 2 3; do q 0 "SELECT current_setting('gp.session_id')"; done | tr '\n' ' ')
	read -r s1 s2 s3 <<< "$out"
	if isnum "$s1" && isnum "$s2" && isnum "$s3" && [ "$s2" -gt "$s1" ] && [ "$s3" -gt "$s2" ]; then
		ok "the sessions' ids are the coordinator's counter's, each past the last ($out)"
	else
		notok "the session ids of three sessions" "$out"
	fi
	out=$(q 0 "SELECT DISTINCT result::int = current_setting('gp.session_id')::int FROM gp.exec_on_segments('SELECT current_setting(''gp.session_id'')');")
	[ "$out" = "t" ] && ok "and a segment's backend of a session works for it" \
		|| notok "gp.session_id on a segment" "$out"

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

	# A gang made while the session was a user who is no superuser -- SET
	# SESSION AUTHORIZATION, a statement, and back -- keeps that user on the
	# segments, and is sent none of the settings only a superuser sets,
	# log_min_messages among them, which its sessions would refuse.
	q 0 "CREATE ROLE sa_u1 LOGIN; CREATE TABLE sa_t (a int) DISTRIBUTED BY (a);
	     INSERT INTO sa_t VALUES (1), (2), (3); GRANT SELECT ON sa_t TO sa_u1;" >/dev/null
	out=$(printf '%s\n' "SET SESSION AUTHORIZATION sa_u1;" "BEGIN;" "SELECT count(*) FROM sa_t;" \
		"RESET SESSION AUTHORIZATION;" "END;" "SELECT count(*) FROM sa_t;" | qf 0 | tr '\n' ' ')
	[ "$out" = "3 3 " ] \
		&& ok "a gang made as a user who is no superuser is sent no superuser's setting once the session is a superuser's again" \
		|| notok "the settings after SET SESSION AUTHORIZATION and back" "$out"
	q 0 "DROP TABLE sa_t; DROP ROLE sa_u1;" >/dev/null

	# gp_sql makes each partition as a statement of its own, with the
	# parent's text; what travels is the tree, so each one arrives as itself.
	q 0 "CREATE EXTENSION gp_sql;" >/dev/null
	q 0 "CREATE TABLE pt (a int, d int) PARTITION BY RANGE (d) (START (1) END (4) EVERY (1));" >/dev/null
	same_everywhere "a classic partitioned table: gp_sql's partitions, made once each" \
		"SELECT string_agg(c.relname || '=' || c.oid, ',' ORDER BY c.relname) FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid WHERE i.inhparent = 'pt'::regclass"

	q 0 "DROP VIEW v1; DROP TABLE d1;" >/dev/null
	same_everywhere "DROP follows" \
		"SELECT count(*) FROM pg_class WHERE relname IN ('d1', 'v1', 'd1_pkey')"

	# ALTER SYSTEM is every node's, as Cloudberry dispatches it: each node's
	# postgresql.auto.conf, outside a transaction, as it runs.
	q 0 "ALTER SYSTEM SET work_mem = '5MB';" >/dev/null
	same_everywhere "ALTER SYSTEM writes every node's postgresql.auto.conf" \
		"SELECT setting FROM pg_file_settings WHERE name = 'work_mem' AND sourcefile LIKE '%postgresql.auto.conf'"
	q 0 "ALTER SYSTEM RESET work_mem;" >/dev/null
	same_everywhere "and RESET takes it out of each" \
		"SELECT count(*) FROM pg_file_settings WHERE name = 'work_mem' AND sourcefile LIKE '%postgresql.auto.conf'"

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

	# A tablespace is each node's directory of its dbid under the location,
	# as Cloudberry's is, linked by the node itself (O32); a table in it has
	# its rows on the segments; every node says the location, and pg_dumpall
	# writes it; and DROP TABLESPACE takes the directories away.
	mkdir -p "$ROOT/tblspc"
	out=$(q 0 "CREATE TABLESPACE ts1 LOCATION '$ROOT/tblspc';")
	[ -z "$out" ] && ok "CREATE TABLESPACE runs" || notok "CREATE TABLESPACE" "$out"
	same_everywhere "and the tablespace has one OID everywhere" \
		"SELECT oid::text FROM pg_tablespace WHERE spcname = 'ts1'"
	ts=$(q 0 "SELECT oid FROM pg_tablespace WHERE spcname = 'ts1';")
	links=$(for n in 0 1 2; do readlink "$(datadir "$n")/pg_tblspc/$ts"; done |
			sed "s#^$ROOT/tblspc/##" | tr '\n' ' ')
	q 0 "CREATE TABLE tsp (a int) TABLESPACE ts1; INSERT INTO tsp SELECT generate_series(1, 100);" >/dev/null
	n1=$(q 1 "SELECT count(*) FROM tsp;"); n2=$(q 2 "SELECT count(*) FROM tsp;")
	[ "$links|$((n1 + n2))|$(ls "$ROOT/tblspc" | tr '\n' ' ')" = "1 2 3 |100|1 2 3 " ] \
		&& ok "each node's is the directory of its dbid under the location, and a table's rows are in it" \
		|| notok "a tablespace's directories" "$links / $n1 + $n2 / $(ls "$ROOT/tblspc")"
	out=$(q 0 "SELECT pg_tablespace_location(oid) FROM pg_tablespace WHERE spcname = 'ts1';")
	out2=$(q 1 "SELECT pg_tablespace_location(oid) FROM pg_tablespace WHERE spcname = 'ts1';")
	out3=$("$BINDIR/pg_dumpall" -h "$(sockdir 0)" -p "$(port 0)" --tablespaces-only 2>&1 |
		   grep "^CREATE TABLESPACE ts1 ")
	case "$out|$out2|$out3" in
		"$ROOT/tblspc|$ROOT/tblspc|CREATE TABLESPACE ts1 "*"LOCATION '$ROOT/tblspc';")
			ok "every node says the location, as Cloudberry's does, and pg_dumpall writes it" ;;
		*) notok "pg_tablespace_location(), and pg_dumpall's CREATE TABLESPACE" "$out / $out2 / $out3" ;;
	esac
	# gp_tablespace_location() says each node's, with its content id, and
	# the directory of this release under it is PostgreSQL 19's.
	out=$(q 0 "SELECT string_agg(gp_segment_id || ' ' || (tblspc_loc = '$ROOT/tblspc'), ',' ORDER BY gp_segment_id) FROM gp_tablespace_location((SELECT oid FROM pg_tablespace WHERE spcname = 'ts1'));")
	out2=$(q 0 "SELECT pg_ls_dir('pg_tblspc/$ts') = get_tablespace_version_directory_name();")
	[ "$out|$out2" = "-1 true,0 true,1 true|t" ] \
		&& ok "gp_tablespace_location() says each node's location, and get_tablespace_version_directory_name() the directory under it" \
		|| notok "gp_tablespace_location()" "$out / $out2"
	# default_tablespace goes to the segments with a statement, as Cloudberry
	# sends it: a table made under it is in the tablespace on every node.
	qf 0 > /dev/null <<'EOF'
SET default_tablespace = ts1;
CREATE TABLE tsd (a int) DISTRIBUTED BY (a);
EOF
	same_everywhere "a table made under default_tablespace is in it on every node" \
		"SELECT spcname::text FROM pg_class c JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE relname = 'tsd'"
	q 0 "DROP TABLE tsp, tsd;" >/dev/null
	out=$(q 0 "DROP TABLESPACE ts1;")
	[ -z "$out" ] && [ -z "$(ls "$ROOT/tblspc")" ] \
		&& ok "DROP TABLESPACE follows, and takes the directories away" \
		|| notok "DROP TABLESPACE" "$out / $(ls "$ROOT/tblspc")"

	# The size functions are the cluster's: the coordinator's size and every
	# segment's, as Cloudberry adds them (gp_size.c); a view still says
	# pg_relation_size, and a query each segment runs has each one's own.
	q 0 "CREATE TABLE sized (a int, b text); INSERT INTO sized SELECT i, repeat('x', 100) FROM generate_series(1, 5000) i;" >/dev/null
	s1=$(q 1 "SELECT pg_relation_size('sized');"); s2=$(q 2 "SELECT pg_relation_size('sized');")
	out=$(q 0 "SELECT pg_relation_size('sized'), pg_table_size('sized') > pg_relation_size('sized'), pg_total_relation_size('sized') = pg_table_size('sized');")
	out2=$(q 0 "CREATE VIEW sizedv AS SELECT pg_relation_size('sized') AS s; SELECT pg_get_viewdef('sizedv');" | tr -s ' \n' ' ')
	c0=$(q 0 "SELECT pg_relation_size('sized') FROM gp_dist_random('gp_id') WHERE gp_segment_id = 0;")
	case "$out|$out2|$c0" in
		"$((s1 + s2))|t|t|"*"pg_relation_size('sized'::regclass)"*"|$s1")
			ok "the size functions add every segment's size to the coordinator's, as Cloudberry's do" ;;
		*) notok "the size functions on the coordinator" "$out / $out2 / $c0 (segments: $s1, $s2)" ;;
	esac
	# cbdb_relation_size() asks the segments once for many relations, and
	# says what pg_relation_size() says of each; a relation gone is 0.
	out=$(q 0 "SELECT string_agg((r.size = pg_relation_size('sized'))::text || ' ' || r.size, ',') FROM cbdb_relation_size(ARRAY['sized'::regclass::oid, 'sized'::regclass::oid]) r;")
	out2=$(q 0 "SELECT reloid || ' ' || size FROM cbdb_relation_size(ARRAY[0::oid], 'fsm');")
	case "$out|$out2" in
		"true "*",true "*"|0 0") ok "cbdb_relation_size() is the cluster's pg_relation_size() of each relation, and 0 of one gone" ;;
		*) notok "cbdb_relation_size()" "$out / $out2" ;;
	esac

	# Each backend makes its own temporary namespace, so its OID is the one
	# thing that differs; the tables in it do not.
	out=$(printf '%s\n' "SET client_min_messages = warning;" "CREATE TEMP TABLE tmp1 (a int);" "CREATE TEMP TABLE tmp2 (a int);" \
		"SELECT ('tmp1'::regclass::oid = min(result::oid)) AND ('tmp1'::regclass::oid = max(result::oid)) AND count(*) = 2 FROM gp.exec_on_segments('SELECT ''tmp1''::regclass::oid');" \
		"SELECT ('tmp2'::regclass::oid = min(result::oid)) AND count(*) = 2 FROM gp.exec_on_segments('SELECT ''tmp2''::regclass::oid');" | qf 0)
	[ "$out" = "t
t" ] && ok "temporary tables, two of them, have the coordinator's OIDs" \
		|| notok "temporary tables" "$out"

	# DISCARD TEMP drops a session's temporary tables on every node, in its
	# transaction, as Cloudberry's does (discard.c); DISCARD ALL drops the
	# coordinator's alone, and says so, where it may run at all.
	tcount="SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM pg_class WHERE relname = ''dtmp'' AND relpersistence = ''t''') WHERE result::int > 0;"
	out=$(printf '%s\n' "SET client_min_messages = warning;" "CREATE TEMP TABLE dtmp (a int);" \
		"BEGIN;" "DISCARD TEMP;" "ROLLBACK;" "$tcount" "DISCARD TEMP;" "$tcount" \
		"CREATE TEMP TABLE dtmp (a int);" "SELECT 'made again';" | qf 0)
	[ "$out" = "2
0
made again" ] && ok "DISCARD TEMP drops the segments' temporary tables too, and a ROLLBACK keeps them" \
		|| notok "DISCARD TEMP" "$out"
	out=$(printf '%s\n' "CREATE TEMP TABLE dall (a int);" "DISCARD ALL;" "BEGIN;" "DISCARD ALL;" "ROLLBACK;" \
		"CREATE TEMP TABLE dall (a int);" | qf 0)
	case "$out" in
		*"NOTICE:  command without clusterwide effect"*"HINT:  Consider alternatives as DEALLOCATE ALL, or DISCARD TEMP if a clusterwide effect is desired."*"ERROR:  DISCARD ALL cannot run inside a transaction block"*'relation "dall" already exists'*)
			[ "$(printf '%s\n' "$out" | grep -c 'without clusterwide effect')" = 1 ] \
				&& ok "DISCARD ALL is the coordinator's, and says so, as Cloudberry's does" \
				|| notok "DISCARD ALL's NOTICE, once" "$out" ;;
		*) notok "DISCARD ALL" "$out" ;;
	esac

	out=$(q 0 "VACUUM kept;")
	[ -z "$out" ] && ok "VACUUM, which runs outside a transaction block, reaches the segments too" \
		|| notok "VACUUM" "$out"

	# So do the forms of REINDEX and CLUSTER that commit as they go, a
	# transaction a table or a partition, which PostgreSQL runs outside a
	# transaction block, and which the segments refused inside the
	# coordinator's (gp_ddl.c).
	out=$(printf '%s\n' "SET client_min_messages = warning;" \
		"CREATE TABLE rix (a int, b int) DISTRIBUTED BY (a) PARTITION BY RANGE (b);" \
		"CREATE TABLE rix1 PARTITION OF rix FOR VALUES FROM (0) TO (50);" \
		"CREATE TABLE rix2 PARTITION OF rix FOR VALUES FROM (50) TO (100);" \
		"CREATE INDEX rix_b ON rix (b);" "INSERT INTO rix SELECT g, g FROM generate_series(0, 99) g;" \
		"REINDEX SCHEMA public;" "REINDEX INDEX rix_b;" "REINDEX TABLE rix;" \
		"CLUSTER rix USING rix_b;" "CLUSTER;" | qf 0 2>&1)
	out2=$(q 0 "SELECT count(*) FROM rix WHERE b < 50;")
	[ -z "$out" ] && [ "$out2" = 50 ] \
		&& ok "REINDEX SCHEMA, REINDEX and CLUSTER of a partitioned table, and CLUSTER of every table, reach the segments too" \
		|| notok "REINDEX and CLUSTER outside a transaction block" "$out / $out2"
	q 0 "DROP TABLE rix;" >/dev/null

	# DROP INDEX CONCURRENTLY writes nothing before the index goes: the rows
	# of pg_stat_last_operation that name it go as the statement ends, as
	# Cloudberry's index_drop() drops them (gp_metatrack.c).
	out=$(printf '%s\n' "SET client_min_messages = warning;" \
		"CREATE TABLE cix (a int, b text) DISTRIBUTED BY (a);" \
		"CREATE INDEX CONCURRENTLY cix_b ON cix (b);" \
		"SELECT 'cix_b'::regclass::oid AS cix \\gset" \
		"DROP INDEX CONCURRENTLY cix_b;" \
		"SELECT count(*) FROM pg_stat_last_operation WHERE objid = :cix;" \
		"DROP TABLE cix;" | qf 0 2>&1)
	[ "$out" = "0" ] \
		&& ok "DROP INDEX CONCURRENTLY of an index pg_stat_last_operation names, which then names it no more" \
		|| notok "DROP INDEX CONCURRENTLY" "$out"

	# REINDEX SCHEMA records each index it rebuilt, as Cloudberry's
	# reindex_index() does, and pg_stat_operations names it (gp_metatrack.c).
	out=$(printf '%s\n' "SET client_min_messages = warning;" \
		"CREATE SCHEMA rsch;" "CREATE TABLE rsch.t (a int, b int) DISTRIBUTED BY (a);" \
		"CREATE INDEX rsch_b ON rsch.t (b);" "REINDEX SCHEMA rsch;" \
		"SELECT schemaname || ' ' || actionname || ' ' || usestatus FROM pg_stat_operations WHERE objname = 'rsch_b' AND subtype = 'REINDEX';" \
		"DROP SCHEMA rsch CASCADE;" | qf 0 2>&1)
	[ "$out" = "rsch VACUUM CURRENT" ] \
		&& ok "REINDEX SCHEMA records each index it rebuilds, which pg_stat_operations names" \
		|| notok "REINDEX SCHEMA in pg_stat_last_operation" "$out"

	# gp_log_backend_memory_contexts(): each segment's backends of the
	# session log their memory contexts, one segment's too; a session that
	# is none's, none (gp_monitor.c).
	out=$(printf '%s\n' "SET client_min_messages = error;" "SELECT count(*) FROM kept;" \
		"SELECT gp_log_backend_memory_contexts(sess_id), gp_log_backend_memory_contexts(sess_id, 1), gp_log_backend_memory_contexts(0) FROM pg_stat_activity WHERE pid = pg_backend_pid();" | qf 0 2>&1 | tail -1)
	[ "$out" = "2|1|0" ] \
		&& ok "gp_log_backend_memory_contexts() has a session's backends on each segment log their memory contexts" \
		|| notok "gp_log_backend_memory_contexts()" "$out"

	# gp_suboverflowed_backend: a transaction whose subtransactions wrote on
	# every node past the cache of its PGPROC shows on each (gp_monitor.c).
	out=$(printf '%s\n' "SET client_min_messages = warning;" \
		"CREATE TABLE subovf (a int) DISTRIBUTED BY (a);" "BEGIN;" \
		"DO \$\$ BEGIN FOR i IN 1..300 LOOP BEGIN INSERT INTO subovf VALUES (i); CREATE TEMP TABLE subovf_t (a int); DROP TABLE subovf_t; EXCEPTION WHEN others THEN NULL; END; END LOOP; END \$\$;" \
		"SELECT string_agg(segid::text, ',' ORDER BY segid) FROM gp_suboverflowed_backend WHERE pg_backend_pid() = ANY (pids) OR (segid >= 0 AND array_length(pids, 1) > 0);" \
		"COMMIT;" "SELECT count(*) FROM gp_suboverflowed_backend WHERE array_length(pids, 1) > 0;" \
		"DROP TABLE subovf;" | qf 0 2>&1)
	[ "$out" = "-1,0,1
0" ] && ok "gp_suboverflowed_backend shows the transaction's overflowed subtransactions on every node, and none once it commits" \
		|| notok "gp_suboverflowed_backend" "$out"

	# gp_dist_wait_status(): every node's waits in Cloudberry's columns, a
	# statement waiting for a table another session holds among them.
	(printf '%s\n' "BEGIN;" "LOCK TABLE kept IN ACCESS EXCLUSIVE MODE;" "SELECT pg_sleep(6);" "COMMIT;" | qf 0 >/dev/null 2>&1) &
	holder=$!
	sleep 1
	(q 0 "SELECT count(*) FROM kept;" >/dev/null 2>&1) &
	waiter=$!
	sleep 2
	out=$(q 0 "SELECT segid || ' ' || waiter_locktype || ' ' || \"holdTillEndXact\" || ' ' || (waiter_sessionid > 0) || ' ' || (holder_sessionid > 0) FROM gp_dist_wait_status() WHERE waiter_lockmode = 'AccessShareLock';")
	wait "$holder" "$waiter"
	[ "$out" = "-1 relation true true true" ] \
		&& ok "gp_dist_wait_status() shows a statement waiting for a table another session holds, and their sessions" \
		|| notok "gp_dist_wait_status()" "$out"

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

	# A segment's temporary namespace is a catalog row of its own, and takes
	# an OID the coordinator never gives: from the top of the OID space down
	# (gp_ddl.c's local_oid()).  Its own counter would not do -- it runs ahead
	# of the coordinator's with each TOAST value the segment stores, and the
	# coordinator could later give one of its OIDs to a schema, which that
	# segment would refuse as a duplicate.  So, in a new database, which has
	# no temporary namespace yet: TOAST values put segment 0's counter ahead,
	# a temporary table makes segment 0 a namespace, and the coordinator's
	# counter, stepped to where segment 0's is, gives the next schemas the
	# OIDs its counter would have given the namespace.
	qdb() {					# qdb <n> <database> <sql>
		"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d "$2" \
			-c "$3" 2>&1
	}
	q 0 "CREATE DATABASE oids;" >/dev/null
	# A TOAST value takes an OID of its node's counter; the coordinator's
	# runs ahead of a segment's with DDL, so segment 0 is given as many as
	# put it ahead, about half of the rows.
	qdb 0 oids "CREATE TABLE toasted (a int, b text) DISTRIBUTED BY (a);
	            ALTER TABLE toasted ALTER b SET STORAGE EXTERNAL;
	            INSERT INTO toasted SELECT i, repeat('x', 2100) FROM generate_series(1, 100) i;" >/dev/null
	toast=$(qdb 0 oids "SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'toasted';")
	counter=$(qdb 1 oids "SELECT max(chunk_id) FROM $toast;")
	coord=$(qdb 0 oids "SELECT lo_create(0);")
	if isnum "$counter" && isnum "$coord" && [ "$counter" -le $((coord + 100)) ]; then
		qdb 0 oids "INSERT INTO toasted SELECT i, repeat('x', 2100) FROM generate_series(101, 100 + ($coord - $counter + 300) * 5 / 2) i;" >/dev/null
		counter=$(qdb 1 oids "SELECT max(chunk_id) FROM $toast;")
	fi
	qdb 0 oids "SET client_min_messages = warning; CREATE TEMP TABLE tmp_local (a int);" >/dev/null
	out=$(qdb 1 oids "SELECT count(*) FILTER (WHERE oid >= 2147483648) || '/' || count(*) FROM pg_namespace WHERE nspname ~ '^pg_(toast_)?temp_';")
	qdb 0 oids "DO \$\$ BEGIN WHILE lo_create(0) < $counter LOOP END LOOP; END \$\$;" >/dev/null
	out2=$(for s in 1 2 3; do qdb 0 oids "CREATE SCHEMA stepped$s;"; done)
	out3=$(qdb 1 oids "SELECT count(*) FROM pg_namespace WHERE nspname ~ '^stepped' AND oid > $counter AND oid <= $counter + 3;")
	q 0 "DROP DATABASE oids;" >/dev/null
	isnum "$counter" && [ "$counter" -gt "$coord" ] && [ "$out|$out2|$out3" = "2/2||3" ] \
		&& ok "a segment's temporary namespace takes an OID the coordinator never gives, and the coordinator's counter meeting the segment's fails no schema" \
		|| notok "a segment's own OIDs" "counters $coord and $counter / namespaces in the band: $out / $out2 / $out3"

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
		*"Filter: (d.b <> (now())::text)"*"Remote SQL: SELECT b FROM ONLY public.d WHERE (a < 10)"*)
			ok "an immutable condition is evaluated on the segments, now() here, and only b is fetched" ;;
		*) notok "which conditions are sent" "$out" ;;
	esac
	out=$(q 0 "SELECT count(*) FROM d WHERE a < 10 AND b <> now()::text;")
	[ "$out" = "9" ] && ok "and the answer is the same ($out)" || notok "a sent condition's answer" "$out"

	# A segment's rows come a thousand at a time, through the gather's
	# cursor, and the end of a batch's statement can come in a later read
	# than its rows: the segment was then taken to have no more, its first
	# thousand handed out (gp_dispatch.c, gather_poll()).  A race of
	# microseconds, where the segment's send buffer fills as the first
	# batch's statement ends and the end goes in a send of its own -- which
	# rows as greenplum_schedule's rle has them, (c1 int, c2 char(30)), make
	# it do: some four of 3,200 such gathers, in eight sessions side by
	# side, lost every row past the thousandth.
	q 0 "CREATE TABLE gat (c1 int, c2 char(30)) DISTRIBUTED BY (c1); INSERT INTO gat SELECT 1, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' FROM generate_series(1, 10000);" >/dev/null
	for gi in $(seq 400); do
		printf 'SELECT * FROM gat \\g /dev/null\n\\echo :ROW_COUNT\n'
	done > "$ROOT/gat.sql"
	pids=()
	for gs in 1 2 3 4 5 6 7 8; do
		"$PSQL" -X -q -h "$(sockdir 0)" -p "$(port 0)" -d postgres -f "$ROOT/gat.sql" \
			> "$ROOT/gat$gs" 2>&1 &
		pids+=($!)
	done
	wait "${pids[@]}"
	out=$(cat "$ROOT"/gat[1-8] | sort | uniq -c | sed 's/^ *//' | tr '\n' ' ')
	[ "$out" = "3200 10000 " ] && ok "a segment's 10000 rows, in each of 3200 gathers, eight sessions side by side" \
		|| notok "a gather's rows past a segment's first thousand" "$out"
	q 0 "DROP TABLE gat;" >/dev/null

	# A segment's backend that terminates itself as a gather reads it --
	# gp_sync_lc_gucs's query, which terminates the session's backends on
	# every segment -- sends its FATAL after the first batch's end, one row,
	# while its connection is idle: libpq hands such an error to the notice
	# receiver, and the next batch's FETCH finds the connection closed,
	# "server closed the connection unexpectedly", unless the dispatcher kept
	# what the segment said (gp_dispatch.c, last_word_keep()).  With the
	# servers on one CPU, the segments have sent their FATALs before the
	# coordinator reads the batch: without it kept, one time in five or
	# none said so.
	pms=$(for n in 0 1 2; do head -1 "$(datadir "$n")/postmaster.pid"; done)
	cpus=$(taskset -pc $$ 2> /dev/null | sed 's/.*: //')
	if [ -n "$cpus" ]; then
		for pm in $pms; do taskset -pc "${cpus%%[,-]*}" "$pm" > /dev/null; done
	fi
	out=$(for ti in 1 2 3 4 5; do
			q 0 "SELECT pg_terminate_backend(pid) FROM gp_dist_random('pg_stat_activity') WHERE sess_id IN (SELECT sess_id FROM pg_stat_activity WHERE pid = pg_backend_pid());"
		done | grep -c "^ERROR:  terminating connection due to administrator command$")
	if [ -n "$cpus" ]; then
		for pm in $pms; do taskset -pc "$cpus" "$pm" > /dev/null; done
	fi
	[ "$out" = "5" ] && ok "a segment's backend terminated between two batches of a gather: its FATAL is the error, 5 times of 5" \
		|| notok "a segment's FATAL between two batches" "$out of 5"

	# The rows' system columns, as the segment that holds each has them, and
	# Cloudberry's word for a ctid read without its gp_segment_id.
	out=$(q 0 "SELECT count(*) FROM d WHERE gp_segment_id = 0 AND ctid = '(0,1)';")
	out2=$(q 0 "SELECT count(*) FROM d WHERE ctid = '(0,1)';")
	out3=$(q 0 "SELECT count(DISTINCT (gp_segment_id, ctid)) = count(*), bool_and(xmin::text::bigint > 2), bool_and(cmin::text::int >= 0), bool_and(tableoid = 'd'::regclass) FROM d;")
	case "$out|$out2|$out3" in
		"1|"*'NOTICE:  SELECT uses system-defined column "d.ctid" without the necessary companion column "d.gp_segment_id"'*"HINT:"*"2|t|t|t|t")
			ok "ctid, xmin, cmin and tableoid are the segment's, and a ctid without gp_segment_id is noticed, in Cloudberry's words" ;;
		*) notok "system columns of a distributed table" "$out / $out2 / $out3" ;;
	esac
	out=$(q 0 "DELETE FROM d WHERE ctid = '(0,1)';")
	out2=$(q 0 "SELECT x.a FROM d x JOIN d y ON x.ctid = y.ctid AND x.gp_segment_id = y.gp_segment_id WHERE x.a <> y.a;")
	case "$out|$out2" in
		*'ERROR:  DELETE uses system-defined column "d.ctid" without the necessary companion column "d.gp_segment_id"'*"|")
			ok "a DELETE by ctid alone is refused; a join on both is not noticed" ;;
		*) notok "a write by ctid alone" "$out / $out2" ;;
	esac

	# Direct dispatch to the segments some keys hash to: an IN list, an OR.
	k1=$(q 0 "SELECT string_agg(g::text, ',') FROM (SELECT g FROM generate_series(1, 30) g WHERE expected_seg(g, 2) = 1 ORDER BY g LIMIT 2) s;")
	k0=$(q 0 "SELECT min(g) FROM generate_series(1, 30) g WHERE expected_seg(g, 2) = 0;")
	out=$(q 0 "SET gp.test_print_direct_dispatch_info = on; SELECT count(*) FROM d WHERE a IN ($k1);")
	out2=$(q 0 "EXPLAIN (COSTS OFF) SELECT count(*) FROM d WHERE a = ${k1%,*} OR a = ${k1#*,};")
	out3=$(q 0 "SET gp.test_print_direct_dispatch_info = on; SELECT count(*) FROM d WHERE a IN ($k1, $k0);")
	case "$out|$out2|$out3" in
		*"INFO:  (slice 1) Dispatch command to SINGLE content"*"2|"*"Gather Motion 1:1 on d"*"Segment: 1"*"|"*"INFO:  (slice 1) Dispatch command to ALL contents:"*"3")
			ok "keys an IN list or an OR fixes are asked of the segments they hash to" ;;
		*) notok "direct dispatch to some segments" "$out / $out2 / $out3" ;;
	esac

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

	# PL/pgSQL gives a statement one parameter for each of the function's
	# variables, and those it does not read have no type; the segments are
	# told every one's, or they could not take the statement (qp_functions).
	q 0 "CREATE TABLE plu (a int, b text) DISTRIBUTED BY (a); INSERT INTO plu SELECT g, 'p' FROM generate_series(1, 20) g;" >/dev/null
	q 0 "CREATE FUNCTION plu_key(k int, v text) RETURNS bigint LANGUAGE plpgsql AS \$\$ DECLARE n bigint; BEGIN BEGIN UPDATE plu SET b = v WHERE a = k; GET DIAGNOSTICS n = ROW_COUNT; EXCEPTION WHEN unique_violation THEN n := -1; END; RETURN n; END \$\$;" >/dev/null
	q 0 "CREATE FUNCTION plu_rec() RETURNS int LANGUAGE plpgsql AS \$\$ DECLARE r record; BEGIN FOR r IN SELECT g AS a FROM generate_series(11, 13) g LOOP UPDATE plu SET b = 'rec' WHERE a <= r.a AND a > 10; END LOOP; RETURN 0; END \$\$;" >/dev/null
	out=$(printf '%s\n' "SELECT plu_key(5, 'five'), plu_rec();" \
		"SELECT string_agg(a || b, ' ' ORDER BY a) FROM plu WHERE b <> 'p';" | qf 0 | tr '\n' ' ')
	[ "$out" = "1|0 5five 11rec 12rec 13rec " ] \
		&& ok "a PL/pgSQL function's UPDATE is sent with every parameter's type, of the variables it reads and those it does not" \
		|| notok "PL/pgSQL variables in an UPDATE sent as it stands" "$out"

	# An UPDATE of the key moves each row: deleted where it is, its new
	# version inserted where it hashes -- Cloudberry's Split Update.
	q 0 "CREATE TABLE dk (a int, b text) DISTRIBUTED BY (a); INSERT INTO dk SELECT g, 'k' FROM generate_series(1, 100) g;" >/dev/null
	out=$(printf '%s\n' "UPDATE dk SET a = a + 1000 WHERE a > 90;" | "$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d postgres 2>&1)
	out2=$(q 0 "SELECT count(*), sum(a) FROM dk;")
	w1=$(q 1 "SELECT count(*) FROM dk WHERE expected_seg(a, 2) <> 0;")
	w2=$(q 2 "SELECT count(*) FROM dk WHERE expected_seg(a, 2) <> 1;")
	[ "$out|$out2|$w1|$w2" = "UPDATE 10|100|15050|0|0" ] \
		&& ok "an UPDATE of the key moves each row to the segment its new key hashes to" \
		|| notok "an UPDATE of the distribution key" "$out / $out2 / misplaced $w1 $w2"

	# What the segments cannot do as it is written, the coordinator's plan
	# does, and each row it changes is changed on its segment, by its ctid
	# there (gp_explicit.c).
	want=$(q 0 "SELECT count(*) FROM d WHERE a IN (SELECT k FROM d2 WHERE v = 3);")
	out=$(printf '%s\n' "UPDATE d SET b = 'j' || d2.v FROM d2 WHERE d.a = d2.k AND d2.v = 3;" | "$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d postgres 2>&1)
	out2=$(q 0 "SELECT count(*) FROM d WHERE b = 'j3';")
	[ "$out|$out2" = "UPDATE $want|$want" ] && ok "an UPDATE that joins another distributed table changes each row where it is ($want)" \
		|| notok "an UPDATE joining a distributed table" "$out / $out2, want $want"

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

	# A client that asked for another encoding than the database's is sent
	# a segment's text in its own, and the rows it writes reach the segments
	# in the database's: a value's binary form between the nodes is the
	# database's (gp_record.c).  It was read and written as the client's --
	# an A with diaeresis, 0xC4 in LATIN1, reached such a client as the two
	# bytes of its UTF-8, and a row it copied in failed on its segment,
	# "invalid byte sequence for encoding "UTF8": 0xc4".
	out=$(printf '%s\n' "CREATE TABLE enc (a int, t text) DISTRIBUTED BY (a);" \
		"SET client_encoding = 'LATIN1';" \
		"INSERT INTO enc VALUES (1, 'funny char ' || chr(196));" \
		"COPY enc FROM STDIN;" "$(printf '2\tcopied \304')" '\.' \
		"SELECT t FROM enc ORDER BY a;" | qf 0 | od -An -tx1 | tr -d ' \n')
	want=$(printf 'funny char \304\ncopied \304\n' | od -An -tx1 | tr -d ' \n')
	out2=$(q 0 "SELECT string_agg(octet_length(t)::text, ' ' ORDER BY a) FROM enc;")
	[ "$out" = "$want" ] && [ "$out2" = "13 9" ] \
		&& ok "a client's own encoding for a segment's text, and the database's for what it writes there" \
		|| notok "a client encoding not the database's" "$out / $out2"

	# An error a segment raises in rows routed to it names no COPY of the
	# segment's -- the rows travel by COPY -- but where the statement was: an
	# INSERT's none, as Cloudberry's has none, a function's its own lines,
	# and a COPY's the line of its data the row came from.  A COPY's error
	# in the data on the coordinator says so as PostgreSQL's COPY does.
	q 0 "CREATE TABLE cx (a int PRIMARY KEY, b text) DISTRIBUTED BY (a);
	     INSERT INTO cx SELECT g, 'x' FROM generate_series(1, 20) g;" >/dev/null
	out=$(q 0 "INSERT INTO cx SELECT 7, 'dup';")
	out2=$(q 0 "DO \$\$ BEGIN INSERT INTO cx SELECT 8, 'dup'; END \$\$;")
	case "$out|$out2" in
		*COPY*) notok "an INSERT's error on a segment names no COPY" "$out / $out2" ;;
		"ERROR:  duplicate key value violates unique constraint \"cx_pkey\""*"|"*"CONTEXT:  SQL statement \"INSERT INTO cx SELECT 8, 'dup'\""*"PL/pgSQL function inline_code_block line 1 at SQL statement")
			ok "an INSERT's error on a segment names no COPY, a function's its own lines" ;;
		*) notok "an INSERT's error on a segment" "$out / $out2" ;;
	esac
	# And it carries what the segment's error named, and where in its code
	# the segment raised it, as Cloudberry's does: a client reads a unique
	# violation's constraint off the error as it would off one server's.
	out=$(printf '%s\n' '\set VERBOSITY verbose' "INSERT INTO cx VALUES (7, 'dup');" | qf 0)
	case "$out" in
		*"SCHEMA NAME:  public"*"TABLE NAME:  cx"*"CONSTRAINT NAME:  cx_pkey"*"LOCATION:  _bt_check_unique, nbtinsert.c:"*)
			ok "a segment's error carries its schema, table and constraint, and its location there" ;;
		*) notok "what a segment's error names" "$out" ;;
	esac
	# A table of no columns takes rows as any other: the segments' COPY of
	# them has no column list, which COPY does not take empty.
	out=$(q 0 "CREATE TABLE cz ();" 2>&1; q 0 "INSERT INTO cz DEFAULT VALUES;"; q 0 "INSERT INTO cz SELECT FROM generate_series(1, 5);"; q 0 "SELECT count(*) FROM cz;")
	[ "$(echo "$out" | tail -1)" = "6" ] \
		&& ok "a table of no columns takes rows" \
		|| notok "rows of a table of no columns" "$out"
	out=$(printf '%s\n' "COPY cx FROM STDIN;" "30	a" "31	b" "9	dup" "32	c" '\.' | qf 0 | grep CONTEXT)
	out2=$(printf '%s\n' "COPY cx FROM STDIN;" "33	a" "x34	b" '\.' | qf 0 | grep CONTEXT)
	[ "$out" = "CONTEXT:  COPY cx, line 3" ] &&
	[ "$out2" = 'CONTEXT:  COPY cx, line 2, column a: "x34"' ] \
		&& ok "a COPY's error, on a segment or here, names the line of its data" \
		|| notok "a COPY's error context" "$out / $out2"

	# Cloudberry's options of COPY, which PostgreSQL's has not, through
	# gp_exttable's filter: FILL MISSING FIELDS, NEWLINE, and text's ESCAPE,
	# from the client, a file and a program; and a row no partition takes,
	# a data error under SEGMENT REJECT LIMIT.
	q 0 "CREATE EXTENSION IF NOT EXISTS gp_exttable;
	     CREATE TABLE cf (a int, b int, c text) DISTRIBUTED BY (a);
	     CREATE TABLE ce (a text, b int) DISTRIBUTED RANDOMLY;" >/dev/null
	out=$(printf '%s\n' "COPY cf FROM STDIN WITH DELIMITER '|' FILL MISSING FIELDS;" "1|1|one" "2|2" "3" '\.' \
		"COPY cf (c, b) FROM STDIN (DELIMITER '|', FILL_MISSING_FIELDS true);" "four|4" "five" '\.' \
		"COPY cf FROM STDIN WITH FILL MISSING FIELDS;" "" '\.' | qf 0 | sed 's/^psql:<stdin>:[0-9]*: //')
	out2=$(q 0 "SELECT coalesce(a::text, '-') || ',' || coalesce(b::text, '-') || ',' || coalesce(c, '-') FROM cf ORDER BY a, b, c;" | tr '\n' ' ')
	[ "$out2" = "1,1,one 2,2,- 3,-,- -,4,four -,-,five " ] &&
	[ "$out" = 'ERROR:  missing data for column "b", found empty data line
CONTEXT:  COPY cf, line 1: ""' ] \
		&& ok "FILL MISSING FIELDS fills a short line with NULLs, and not an empty one" \
		|| notok "FILL MISSING FIELDS" "$out / $out2"
	out=$(q 0 "COPY cf FROM PROGRAM 'printf \"6|6|six\\\\r\\\\n7|7|seven\\\\r\\\\n\"' WITH DELIMITER '|' NEWLINE 'crlf';
	           COPY cf FROM PROGRAM 'printf \"8|8|eight\\\\r9|9|nine\\\\r\"' WITH DELIMITER '|' NEWLINE 'cr';
	           SELECT string_agg(c, ',' ORDER BY a) FROM cf WHERE a > 5;")
	out2=$(q 0 "COPY cf FROM STDIN WITH NEWLINE 'lf2';")
	out3=$(q 0 "COPY cf TO STDOUT WITH NEWLINE 'lf';")
	[ "$out" = "six,seven,eight,nine" ] &&
	[ "$out2" = 'ERROR:  invalid value for NEWLINE "lf2"
HINT:  Valid options are: '"'LF', 'CRLF' and 'CR'." ] &&
	[ "$out3" = "ERROR:  newline currently available for data loading only, not unloading" ] \
		&& ok "NEWLINE ends a line where it says, and only a COPY FROM takes it" \
		|| notok "NEWLINE" "$out / $out2 / $out3"
	out=$(printf '%s\n' "COPY ce FROM STDIN WITH DELIMITER '|' ESCAPE '#';" "at #100 and #|bar|1" 'one \ back|2' '\.' \
		"COPY ce FROM STDIN WITH DELIMITER '|' ESCAPE 'off';" 'c:\\dir\new|3' '\.' | qf 0)
	q 0 "COPY ce FROM PROGRAM 'printf \"x\\\\\\\\y|4\\\\n\"' WITH DELIMITER '|' ESCAPE 'off';" >/dev/null
	out2=$(q 0 "SELECT string_agg(a, ' / ' ORDER BY b) FROM ce;")
	[ -z "$out" ] && [ "$out2" = 'at @ and |bar / one \ back / c:\\dir\new / x\y' ] \
		&& ok "text's ESCAPE, a character of its own or OFF, from the client and a program" \
		|| notok "text's ESCAPE" "$out / $out2"
	out=$(q 0 "COPY (SELECT * FROM ce WHERE b > 1) TO '$ROOT/ce.txt' WITH DELIMITER '|' ESCAPE 'off';
	           DELETE FROM ce WHERE b > 1;
	           COPY ce FROM '$ROOT/ce.txt' WITH DELIMITER '|' ESCAPE 'off';")
	out2=$(q 0 "SELECT string_agg(a, ' / ' ORDER BY b) FROM ce WHERE b > 1;")
	out3=$(q 0 "COPY (SELECT E'a\\\\b', E'c\\nd|e', NULL::text) TO STDOUT WITH DELIMITER '|' ESCAPE '#';
	            COPY (SELECT E'a\\\\b', NULL::text) TO STDOUT WITH DELIMITER '|' ESCAPE 'off';")
	q 0 "COPY (SELECT 'to a program') TO PROGRAM 'cat > $ROOT/ce_prog.txt' ESCAPE 'OFF';" >/dev/null
	[ -z "$out" ] && [ "$out2" = 'one \ back / c:\\dir\new / x\y' ] &&
	[ "$out3" = 'a#\b|c#nd#|e|\N
a\b|\N' ] && [ "$(cat "$ROOT/ce_prog.txt" 2>&1)" = "to a program" ] \
		&& ok "and COPY TO writes with it, to a file, the client and a program" \
		|| notok "COPY TO with text's ESCAPE" "$out / $out2 / $out3 / $(cat "$ROOT/ce_prog.txt" 2>&1)"
	q 0 "CREATE TABLE cp (i int) DISTRIBUTED BY (i) PARTITION BY RANGE (i) (START (1) END (5) EVERY (1));" >/dev/null
	out=$(printf '%s\n' "COPY cp FROM STDIN LOG ERRORS SEGMENT REJECT LIMIT 10;" "2" "10000" "f" "3" '\.' |
		qf 0 | sed 's/^psql:<stdin>:[0-9]*: //')
	out2=$(q 0 "SELECT string_agg(i::text, ',' ORDER BY i) FROM cp;
	            SELECT string_agg(linenum || ':' || errmsg, ' / ' ORDER BY linenum) FROM gp_read_error_log('cp');")
	[ "$out" = "NOTICE:  found 2 data formatting errors (2 or more input rows), rejected related input data" ] &&
	[ "$out2" = '2,3
2:no partition of relation "cp" found for row / 3:invalid input syntax for type integer: "f", column i' ] \
		&& ok "a row no partition takes is a data error under SEGMENT REJECT LIMIT, logged with its line" \
		|| notok "SEGMENT REJECT LIMIT and a row no partition takes" "$out / $out2"

	out=$(q 0 "SELECT count(*) FROM (SELECT a FROM d WHERE a < 5 FOR UPDATE) s;")
	[ "$out" = "4" ] && ok "SELECT ... FOR UPDATE locks the table, as Cloudberry does without GDD" \
		|| notok "SELECT FOR UPDATE" "$out"

	out=$(printf '%s\n' "BEGIN;" "INSERT INTO d VALUES (800, 'r'), (801, 's') RETURNING a, b || '!';" \
		"SELECT count(*) FROM d WHERE a >= 800;" "ROLLBACK;" | qf 0 | tr '\n' ' ')
	[ "$out" = "800|r! 801|s! 2 " ] && ok "INSERT ... RETURNING: the rows each segment wrote come back" \
		|| notok "INSERT RETURNING" "$out"

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

	# A write in a WITH query, and one of a partitioned table's partitions:
	# the plan runs here, and every row goes to its segment.
	out=$(q 0 "WITH w AS (UPDATE d SET b = b WHERE a < 3 RETURNING a) SELECT count(*), sum(a) FROM w;")
	[ "$out" = "2|3" ] && ok "an UPDATE in a WITH query, its RETURNING read by the query" \
		|| notok "a data-modifying WITH query" "$out"
	# INSERT ... ON CONFLICT in a WITH query: each one's clause, printed with
	# the statement, goes with its own subplan's rows -- swapped, upw would
	# keep 'one' and upw2 take 'eins'.
	q 0 "CREATE TABLE upw (a int PRIMARY KEY, b text) DISTRIBUTED BY (a); INSERT INTO upw VALUES (1, 'one'), (2, 'two');" >/dev/null
	q 0 "CREATE TABLE upw2 (a int PRIMARY KEY, b text) DISTRIBUTED BY (a); INSERT INTO upw2 VALUES (1, 'x');" >/dev/null
	out=$(q 0 "WITH w AS (INSERT INTO upw VALUES (2, 'deux'), (3, 'trois') ON CONFLICT (a) DO UPDATE SET b = excluded.b || '!' RETURNING a, b) SELECT string_agg(a || b, ' ' ORDER BY a) FROM w;")
	out2=$(q 0 "WITH x AS (INSERT INTO upw VALUES (1, 'uno') ON CONFLICT (a) DO UPDATE SET b = excluded.b RETURNING b), u AS (UPDATE upw SET b = b WHERE a = 3 RETURNING a), y AS (INSERT INTO upw2 VALUES (1, 'eins'), (2, 'zwei') ON CONFLICT DO NOTHING RETURNING b) SELECT (SELECT string_agg(b, ',') FROM x), (SELECT count(*) FROM u), (SELECT string_agg(b, ',') FROM y);")
	out3=$(q 0 "SELECT (SELECT string_agg(a || b, ' ' ORDER BY a) FROM upw), (SELECT string_agg(a || b, ' ' ORDER BY a) FROM upw2);")
	[ "$out|$out2|$out3" = "2deux! 3trois|uno|1|zwei|1uno 2deux! 3trois|1x 2zwei" ] \
		&& ok "INSERT ... ON CONFLICT in a WITH query, two of them, each with its own clause" \
		|| notok "ON CONFLICT in a WITH query" "$out / $out2 / $out3"
	out=$(printf '%s\n' "BEGIN;" "DELETE FROM sales WHERE amt <= 3;" \
		"SELECT count(*) FROM sales;" \
		"UPDATE sales SET d = date '2026-03-20' WHERE id BETWEEN 10 AND 12 RETURNING id, tableoid::regclass;" \
		"SELECT count(*) FROM sales_1_prt_3 WHERE id BETWEEN 10 AND 12;" "ROLLBACK;" | qf 0 | tr '\n' ' ')
	[ "$out" = "87 10|sales_1_prt_3 11|sales_1_prt_3 12|sales_1_prt_3 3 " ] \
		&& ok "a partitioned table's DELETE, and an UPDATE that moves rows to another partition" \
		|| notok "writes of a partitioned table" "$out"
	# ... sent to the segments whole, as a plain table's, where it reads only
	# the table and every table it writes is distributed as the one it names
	# -- the planner's one partition alone, or several: each segment finds
	# each row's partition, and moves a row whose partition key changes into
	# another, one whose columns are in another order among them.  The rows
	# stay on their segments, and come out as an unpartitioned twin's do.  A
	# partition is always distributed as its table; an inheritance child
	# distributed otherwise leaves the statement to the explicit write.
	q 0 "CREATE TABLE pdw (a int, b int, c int) DISTRIBUTED BY (a) PARTITION BY RANGE (b);
	     CREATE TABLE pdw_1 PARTITION OF pdw FOR VALUES FROM (0) TO (100);
	     CREATE TABLE pdw_2 PARTITION OF pdw FOR VALUES FROM (100) TO (200);
	     CREATE TABLE pdw_3 (c int, b int, a int) DISTRIBUTED BY (a);
	     ALTER TABLE pdw ATTACH PARTITION pdw_3 FOR VALUES FROM (200) TO (300);
	     CREATE TABLE pdw_twin (a int, b int, c int) DISTRIBUTED BY (a);
	     INSERT INTO pdw SELECT g, g % 300, g FROM generate_series(1, 3000) g;
	     INSERT INTO pdw_twin SELECT * FROM pdw;" >/dev/null 2>&1
	plans=""
	for sql in "UPDATE %s SET c = c + 1 WHERE b < 50;" "UPDATE %s SET b = b + 150 WHERE b >= 50 AND b < 150;" \
		"DELETE FROM %s WHERE a %% 10 = 0;"; do
		# shellcheck disable=SC2059
		plans="$plans$(q 0 "EXPLAIN (COSTS OFF) $(printf "$sql" pdw)" | head -1)|"
		# shellcheck disable=SC2059
		q 0 "$(printf "$sql" pdw) $(printf "$sql" pdw_twin)" >/dev/null
	done
	out=$(q 0 "SELECT string_agg(k || ':' || n || ':' || s, ' ' ORDER BY k) FROM (SELECT b / 100 AS k, count(*) n, sum(c) s FROM pdw GROUP BY 1) x;")
	out2=$(q 0 "SELECT string_agg(k || ':' || n || ':' || s, ' ' ORDER BY k) FROM (SELECT b / 100 AS k, count(*) n, sum(c) s FROM pdw_twin GROUP BY 1) x;")
	w1=$(q 1 "SELECT count(*) FROM pdw WHERE expected_seg(a, 2) <> 0;"); w2=$(q 2 "SELECT count(*) FROM pdw WHERE expected_seg(a, 2) <> 1;")
	q 0 "CREATE TABLE pdw_inh (a int, b int) DISTRIBUTED BY (a); CREATE TABLE pdw_inh_c (c int) INHERITS (pdw_inh) DISTRIBUTED BY (b);" >/dev/null 2>&1
	plan=$(q 0 "EXPLAIN (COSTS OFF) DELETE FROM pdw_inh WHERE a % 10 = 1;" | head -1)
	[ "$plans|$out|$w1|$w2|$plan" = "Custom Scan (Dispatch)|Custom Scan (Dispatch)|Custom Scan (Dispatch)||$out2|0|0|Custom Scan (Explicit Redistribute Motion)" ] \
		&& [ "$out2" = "0:450:619200 1:450:686250 2:1800:2745000" ] \
		&& ok "... sent to the segments whole where each table it writes is distributed as the one it names, rows moved between partitions there" \
		|| notok "a partitioned table's UPDATE and DELETE sent to the segments" "$plans / $out / twin $out2 / misplaced $w1 $w2 / $plan"
	# RETURNING old and new by name: each row comes back with its other
	# version -- an UPDATE's old row, an upsert's existing one, a moved row's
	# deleted one -- and one that is not there is null.
	q 0 "CREATE TABLE ron (a int, b text, c int GENERATED ALWAYS AS (a * 10) STORED) DISTRIBUTED BY (a); INSERT INTO ron SELECT g, 'v' || g FROM generate_series(1, 10) g;" >/dev/null
	q 0 "CREATE TABLE ronu (a int PRIMARY KEY, b text) DISTRIBUTED BY (a); INSERT INTO ronu VALUES (1, 'one');" >/dev/null
	out=$(printf '%s\n' "BEGIN;" \
		"UPDATE ron SET b = b || '!' WHERE a = 2 RETURNING old.b, new.b;" \
		"WITH w AS (DELETE FROM ron WHERE a = 10 RETURNING old.a AS o, new.a AS n) SELECT o, n IS NULL FROM w;" \
		"INSERT INTO ron VALUES (11, 'x') RETURNING old.a IS NULL, new.c;" \
		"WITH w AS (INSERT INTO ronu VALUES (1, 'uno'), (2, 'dos') ON CONFLICT (a) DO UPDATE SET b = excluded.b RETURNING old.b AS o, new.b AS n) SELECT string_agg(coalesce(o, '-') || '>' || n, ' ' ORDER BY n) FROM w;" \
		"UPDATE ron SET a = a + 100 WHERE a = 4 RETURNING old.a, new.a, old.c, new.c;" \
		"UPDATE sales SET d = date '2026-03-20' WHERE id = 10 RETURNING old.tableoid::regclass, new.tableoid::regclass;" \
		"ROLLBACK;" | qf 0 | tr '\n' ' ')
	[ "$out" = "v2|v2! 10|t t|110 ->dos one>uno 4|104|40|1040 sales_1_prt_1|sales_1_prt_3 " ] \
		&& ok "RETURNING old and new: an UPDATE's, a DELETE's, an INSERT's, an upsert's, a moved row's, a row moved between partitions" \
		|| notok "RETURNING old and new" "$out"
	# RETURNING ctid: each row's own, on the segment that wrote it -- an
	# UPDATE's new version and its old one, a moved row's, an INSERT's, a
	# DELETE's row -- as a segment's own ModifyTable gives it.
	q 0 "CREATE TABLE rct (a int, b int) DISTRIBUTED BY (a); INSERT INTO rct SELECT g, g FROM generate_series(1, 20) g;" >/dev/null
	before=$(for n in 1 2; do q $n "SELECT ctid FROM rct WHERE a = 6;"; done)
	out=$( { q 0 "UPDATE rct SET b = -b WHERE a IN (3, 4) RETURNING a, gp_segment_id, ctid, old.ctid <> new.ctid;"
	         q 0 "UPDATE rct SET a = a + 100 WHERE a = 5 RETURNING a, gp_segment_id, ctid, old.ctid <> new.ctid;"
	         q 0 "INSERT INTO rct VALUES (30, 30) RETURNING a, gp_segment_id, ctid, true;"; } | sort)
	out2=$(q 0 "DELETE FROM rct WHERE a = 6 RETURNING ctid;")
	bad=""
	while IFS='|' read -r a seg ctid moved; do
		isnum "$seg" && [ "$(q $((seg + 1)) "SELECT ctid FROM rct WHERE a = $a;")" = "$ctid" ] && [ "$moved" = t ] \
			|| bad="$bad $a"
	done <<< "$out"
	[ "$(echo "$out" | wc -l)" = 4 ] && [ -z "$bad" ] && [ -n "$before" ] && [ "$out2" = "$before" ] \
		&& ok "RETURNING ctid: each row's, on the segment that wrote it" \
		|| notok "RETURNING ctid" "$out / not the segment's:$bad / deleted $out2, was $before"
	# Check options.  A view's WITH CHECK OPTION is the coordinator's: the
	# rows the segments wrote come back and are checked here -- an INSERT's,
	# an UPDATE's, a moved row's, an upsert's -- LOCAL and CASCADED as
	# PostgreSQL has them.
	q 0 "CREATE TABLE wco (a int, b int) DISTRIBUTED BY (a); INSERT INTO wco SELECT g, g FROM generate_series(1, 10) g;" >/dev/null
	q 0 "CREATE VIEW wcov AS SELECT * FROM wco WHERE b < 100 WITH CHECK OPTION; CREATE VIEW wcov2 AS SELECT * FROM wcov WHERE b > 0 WITH LOCAL CHECK OPTION;" >/dev/null
	q 0 "CREATE TABLE wcu (a int PRIMARY KEY, b int) DISTRIBUTED BY (a); INSERT INTO wcu VALUES (1, 1), (2, 2); CREATE VIEW wcuv AS SELECT * FROM wcu WHERE b < 100 WITH CHECK OPTION;" >/dev/null
	out=$(printf '%s\n' "INSERT INTO wcov VALUES (11, 11);" "INSERT INTO wcov VALUES (12, 200);" \
		"UPDATE wcov SET b = b + 5 WHERE a = 1;" "UPDATE wcov SET b = 500 WHERE a = 2;" \
		"UPDATE wcov SET a = a + 100 WHERE a = 3;" "UPDATE wcov SET a = a + 100, b = 300 WHERE a = 4;" \
		"INSERT INTO wcov2 VALUES (13, -1);" "INSERT INTO wcov2 VALUES (14, 150);" \
		"INSERT INTO wcuv VALUES (1, 500) ON CONFLICT (a) DO UPDATE SET b = excluded.b;" \
		"INSERT INTO wcuv VALUES (3, 300) ON CONFLICT (a) DO UPDATE SET b = excluded.b;" \
		"INSERT INTO wcuv VALUES (2, 20), (4, 40) ON CONFLICT (a) DO UPDATE SET b = excluded.b;" | qf 0)
	n1=$(printf '%s\n' "$out" | grep -c 'ERROR:  new row violates check option for view "wcov"')
	n2=$(printf '%s\n' "$out" | grep -c 'ERROR:  new row violates check option for view "wcov2"')
	n3=$(printf '%s\n' "$out" | grep -c 'ERROR:  new row violates check option for view "wcuv"')
	n4=$(printf '%s\n' "$out" | grep -c 'DETAIL:  Failing row contains (104, 300).')
	out2=$(q 0 "SELECT (SELECT string_agg(a || ':' || b, ' ' ORDER BY a) FROM wco), (SELECT string_agg(a || ':' || b, ' ' ORDER BY a) FROM wcu);")
	[ "$n1|$n2|$n3|$n4|$out2" = "4|1|2|1|1:6 2:2 4:4 5:5 6:6 7:7 8:8 9:9 10:10 11:11 103:3|1:1 2:20 4:40" ] \
		&& ok "a view's WITH CHECK OPTION, checked over the rows the segments wrote: inserted, updated, moved, upserted" \
		|| notok "WITH CHECK OPTION" "$n1 $n2 $n3 $n4 / $out2 / $out"
	# A table's policies: a segment's statement runs as the session's role,
	# and applies them as its own; the Split's new row is checked here.
	q 0 "CREATE ROLE rls_w LOGIN; CREATE TABLE rlt (a int, b int, owner text) DISTRIBUTED BY (a); INSERT INTO rlt VALUES (1, 1, 'rls_w'), (2, 2, 'other'), (3, 3, 'rls_w'); GRANT SELECT, INSERT, UPDATE, DELETE ON rlt TO rls_w;" >/dev/null
	q 0 "ALTER TABLE rlt ENABLE ROW LEVEL SECURITY; CREATE POLICY rlt_s ON rlt FOR SELECT USING (true); CREATE POLICY rlt_i ON rlt FOR INSERT WITH CHECK (owner = current_user); CREATE POLICY rlt_u ON rlt FOR UPDATE USING (owner = current_user) WITH CHECK (b < 50); CREATE POLICY rlt_d ON rlt FOR DELETE USING (owner = current_user);" >/dev/null
	out=$(printf '%s\n' "SET ROLE rls_w;" "INSERT INTO rlt VALUES (4, 4, 'rls_w');" "INSERT INTO rlt VALUES (5, 5, 'other');" \
		"UPDATE rlt SET b = b + 10;" "UPDATE rlt SET b = 99 WHERE a = 1;" "UPDATE rlt SET a = a + 100 WHERE a = 3;" \
		"UPDATE rlt SET a = a + 200, b = 60 WHERE a = 4;" "DELETE FROM rlt WHERE a = 2;" | qf 0)
	n1=$(printf '%s\n' "$out" | grep -c 'ERROR:  new row violates row-level security policy for table "rlt"')
	out2=$(q 0 "SELECT string_agg(a || ':' || b, ' ' ORDER BY a) FROM rlt;")
	[ "$n1|$out2" = "3|1:11 2:2 4:14 103:13" ] \
		&& ok "a table's policies under a role not its owner: INSERT, UPDATE, a moved row, DELETE" \
		|| notok "row-level security" "$n1 / $out2 / $out"

	# MERGE: the plan joins source and target here, the target's row with
	# it, and each action's row is written where it is (gp_explicit.c).
	q 0 "CREATE TABLE mt (k int, v int, note text) DISTRIBUTED BY (k); INSERT INTO mt SELECT g, g * 10, 'n' || g FROM generate_series(1, 10) g;" >/dev/null
	q 0 "CREATE TABLE ms (k int, v int) DISTRIBUTED BY (k); INSERT INTO ms VALUES (1, 1), (2, 2), (3, 3), (11, 11), (12, 12);" >/dev/null
	out=$(q 0 "MERGE INTO mt t USING ms s ON t.k = s.k WHEN MATCHED AND t.v > 25 THEN DELETE WHEN MATCHED AND s.v = 1 THEN DO NOTHING WHEN MATCHED THEN UPDATE SET v = t.v + s.v, note = t.note || '+' WHEN NOT MATCHED AND s.k = 12 THEN DO NOTHING WHEN NOT MATCHED THEN INSERT (k, v, note) VALUES (s.k, s.v, 'new') WHEN NOT MATCHED BY SOURCE AND t.k > 8 THEN UPDATE SET note = 'orphan' WHEN NOT MATCHED BY SOURCE AND t.k = 4 THEN DELETE;")
	out2=$(q 0 "SELECT string_agg(k || ':' || v || ':' || note, ' ' ORDER BY k) FROM mt;")
	[ "$out|$out2" = "|1:10:n1 2:22:n2+ 5:50:n5 6:60:n6 7:70:n7 8:80:n8 9:90:orphan 10:100:orphan 11:11:new" ] \
		&& ok "MERGE: MATCHED, NOT MATCHED and NOT MATCHED BY SOURCE, each action where its row is, its WHEN over the target's row" \
		|| notok "MERGE" "$out / $out2"
	q 0 "INSERT INTO ms VALUES (2, 20);" >/dev/null
	out=$(q 0 "MERGE INTO mt t USING ms s ON t.k = s.k WHEN MATCHED THEN UPDATE SET v = s.v;")
	out2=$(q 0 "MERGE INTO mt t USING ms s ON t.k = s.k WHEN MATCHED AND s.v = 20 THEN DO NOTHING WHEN MATCHED THEN UPDATE SET v = s.v; SELECT string_agg(k || ':' || v, ' ' ORDER BY k) FROM mt WHERE k <= 3;")
	out3=$(q 0 "MERGE INTO mt t USING ms s ON t.k = s.k WHEN MATCHED THEN UPDATE SET v = 0 RETURNING merge_action();")
	case "$out|$out2|$out3" in
		*"MERGE command cannot affect row a second time"*"|1:1 2:2|"*"cannot MERGE INTO distributed table \"mt\" this way yet"*)
			ok "a row an action changes twice is refused, one it passes over is not; RETURNING is refused" ;;
		*) notok "MERGE's refusals" "$out / $out2 / $out3" ;;
	esac
	q 0 "CREATE TABLE mr (k int, v text) DISTRIBUTED REPLICATED; INSERT INTO mr VALUES (1, 'a'), (2, 'b'), (3, 'c'); CREATE VIEW mtv AS SELECT * FROM mt WHERE v < 1000 WITH CHECK OPTION;" >/dev/null
	out=$(q 0 "MERGE INTO mr t USING (VALUES (1, 'x'), (2, NULL), (4, 'd')) AS s(k, v) ON t.k = s.k WHEN MATCHED AND s.v IS NULL THEN DELETE WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED THEN INSERT VALUES (s.k, s.v);")
	n1=$(q 1 "SELECT string_agg(k || v, ' ' ORDER BY k) FROM mr;"); n2=$(q 2 "SELECT string_agg(k || v, ' ' ORDER BY k) FROM mr;")
	out2=$(printf '%s\n' "MERGE INTO mtv t USING (VALUES (5, 5000)) s(k, v) ON t.k = s.k WHEN MATCHED THEN UPDATE SET v = s.v;" \
		"MERGE INTO mtv t USING (VALUES (50, 5000)) s(k, v) ON t.k = s.k WHEN NOT MATCHED THEN INSERT (k, v) VALUES (s.k, s.v);" | qf 0 | grep -c 'ERROR:  new row violates check option for view "mtv"')
	[ "$out|$n1|$n2|$out2" = "|1x 3c 4d|1x 3c 4d|2" ] \
		&& ok "MERGE into a replicated table, every copy alike; through a view WITH CHECK OPTION, its UPDATE and INSERT checked" \
		|| notok "MERGE into a replicated table, through a view" "$out / $n1 / $n2 / $out2"
	q 0 "CREATE TABLE mpa (id int, d int, v text) DISTRIBUTED BY (id) PARTITION BY RANGE (d) (START (0) END (30) EVERY (10)); INSERT INTO mpa SELECT g, g, 'v' || g FROM generate_series(1, 25) g;" >/dev/null
	out=$(q 0 "MERGE INTO mpa t USING (VALUES (5, 'five'), (15, 'fifteen'), (27, 'new')) s(id, v) ON t.id = s.id WHEN MATCHED AND t.id = 5 THEN UPDATE SET d = 22, v = s.v WHEN MATCHED THEN UPDATE SET id = t.id + 100, v = s.v WHEN NOT MATCHED THEN INSERT VALUES (s.id, 27, s.v);")
	out2=$(q 0 "SELECT string_agg(id || ':' || d || ':' || v || ':' || tableoid::regclass, ' ' ORDER BY id) FROM mpa WHERE id IN (5, 15, 27, 115);")
	w1=$(q 1 "SELECT count(*) FROM mpa WHERE expected_seg(id, 2) <> 0;"); w2=$(q 2 "SELECT count(*) FROM mpa WHERE expected_seg(id, 2) <> 1;")
	[ "$out|$out2|$w1|$w2" = "|5:22:five:mpa_1_prt_3 27:27:new:mpa_1_prt_3 115:15:fifteen:mpa_1_prt_2|0|0" ] \
		&& ok "MERGE into a partitioned table: a row moved between partitions, one between segments, one routed" \
		|| notok "MERGE into a partitioned table" "$out / $out2 / misplaced $w1 $w2"
	q 0 "CREATE TABLE mpol (k int, v int, owner text) DISTRIBUTED BY (k); INSERT INTO mpol VALUES (1, 1, 'rls_w'), (2, 2, 'other'); GRANT SELECT, INSERT, UPDATE ON mpol TO rls_w; ALTER TABLE mpol ENABLE ROW LEVEL SECURITY; CREATE POLICY mpol_s ON mpol FOR SELECT USING (true); CREATE POLICY mpol_u ON mpol FOR UPDATE USING (owner = current_user) WITH CHECK (v < 100); CREATE POLICY mpol_i ON mpol FOR INSERT WITH CHECK (owner = current_user);" >/dev/null
	out=$(printf '%s\n' "SET ROLE rls_w;" \
		"MERGE INTO mpol t USING (VALUES (1, 10)) s(k, v) ON t.k = s.k WHEN MATCHED THEN UPDATE SET v = s.v;" \
		"MERGE INTO mpol t USING (VALUES (2, 20)) s(k, v) ON t.k = s.k WHEN MATCHED THEN UPDATE SET v = s.v;" \
		"MERGE INTO mpol t USING (VALUES (1, 500)) s(k, v) ON t.k = s.k WHEN MATCHED THEN UPDATE SET v = s.v;" \
		"MERGE INTO mpol t USING (VALUES (3, 3)) s(k, v) ON t.k = s.k WHEN NOT MATCHED THEN INSERT VALUES (s.k, s.v, 'other');" \
		"MERGE INTO mpol t USING (VALUES (3, 3)) s(k, v) ON t.k = s.k WHEN NOT MATCHED THEN INSERT VALUES (s.k, s.v, 'rls_w');" | qf 0)
	n1=$(printf '%s\n' "$out" | grep -c 'ERROR:  target row violates row-level security policy (USING expression) for table "mpol"')
	n2=$(printf '%s\n' "$out" | grep -c 'ERROR:  new row violates row-level security policy for table "mpol"')
	out2=$(q 0 "SELECT string_agg(k || ':' || v || ':' || owner, ' ' ORDER BY k) FROM mpol;")
	[ "$n1|$n2|$out2" = "1|2|1:10:rls_w 2:2:other 3:3:rls_w" ] \
		&& ok "MERGE under a table's policies: the target's row against the UPDATE's USING, the new rows against WITH CHECK" \
		|| notok "MERGE and row-level security" "$n1 $n2 / $out2 / $out"
	# A write in a WITH query runs to its end, whether or not the query reads
	# what it returns, as PostgreSQL runs one.
	q 0 "CREATE TABLE cw (k int, v int) DISTRIBUTED BY (k); INSERT INTO cw SELECT g, g FROM generate_series(1, 6) g;" >/dev/null
	out=$(printf '%s\n' "WITH u AS (UPDATE cw SET v = -v WHERE k <= 2) SELECT 1;" \
		"WITH d AS (DELETE FROM cw WHERE k = 3) SELECT 2;" "WITH i AS (INSERT INTO cw VALUES (11, 11)) SELECT 3;" \
		"WITH m AS (MERGE INTO cw t USING (VALUES (4, 44), (12, 12)) s(k, v) ON t.k = s.k WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED THEN INSERT VALUES (s.k, s.v)) SELECT 4;" \
		"WITH u AS (UPDATE cw SET v = v + 1000 WHERE k = 5 RETURNING k) SELECT 5 FROM u LIMIT 0;" \
		"SELECT string_agg(k || ':' || v, ' ' ORDER BY k) FROM cw;" | qf 0 | tr '\n' ' ')
	[ "$out" = "1 2 3 4 1:-1 2:-2 4:44 5:1005 6:6 11:11 12:12 " ] \
		&& ok "a write in a WITH query the query does not read runs to its end: UPDATE, DELETE, INSERT, MERGE" \
		|| notok "unread writes in WITH queries" "$out"
	out=$(printf '%s\n' "BEGIN;" \
		"DELETE FROM d USING d2 WHERE d.a = d2.v * 10 AND d2.v > 4 RETURNING d.a, d2.v;" "ROLLBACK;" | qf 0 | sort -u | tr '\n' ' ')
	[ "$out" = "50|5 60|6 " ] && ok "a DELETE that joins another distributed table, its RETURNING reading both" \
		|| notok "DELETE ... USING ... RETURNING" "$out"
	# d2 has each value of v some fourteen times: a row of dk it finds is
	# found fourteen times, and the second of its new versions could not be
	# taken back -- Cloudberry's Split refuses it, and so does this.
	out=$(q 0 "UPDATE dk SET a = dk.a + 5000 FROM d2 WHERE dk.a = d2.v RETURNING dk.a;")
	case "$out" in
		*"multiple updates to a row by the same query is not allowed"*)
			ok "an UPDATE of the key joining a distributed table that finds a row twice is refused, as Cloudberry refuses it" ;;
		*) notok "an UPDATE of the key that finds a row twice" "$out" ;;
	esac
	q 0 "CREATE TABLE dku (v int) DISTRIBUTED BY (v); INSERT INTO dku SELECT generate_series(1, 6);" >/dev/null
	out=$(q 0 "UPDATE dk SET a = dk.a + 5000 FROM dku WHERE dk.a = dku.v RETURNING dk.a;" | sort -n | tr '\n' ' ')
	out2=$(q 0 "SELECT count(*), sum(a) FROM dk;")
	w1=$(q 1 "SELECT count(*) FROM dk WHERE expected_seg(a, 2) <> 0;")
	w2=$(q 2 "SELECT count(*) FROM dk WHERE expected_seg(a, 2) <> 1;")
	[ "$out|$out2|$w1|$w2" = "5001 5002 5003 5004 5005 5006 |100|45050|0|0" ] \
		&& ok "and one that joins another distributed table, each row moved once, its RETURNING the new row" \
		|| notok "an UPDATE of the key joining a distributed table" "$out / $out2 / misplaced $w1 $w2"
	q 0 "CREATE TABLE dkt (a int, b int) DISTRIBUTED BY (a); CREATE FUNCTION dkt_f() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RETURN NEW; END \$\$; CREATE TRIGGER dkt_t BEFORE UPDATE ON dkt FOR EACH ROW EXECUTE FUNCTION dkt_f();" >/dev/null
	out=$(q 0 "UPDATE dkt SET a = a + 1;")
	case "$out" in
		*"UPDATE on distributed key column not allowed on relation with update triggers"*)
			ok "the key of a table with UPDATE triggers is refused, as Cloudberry refuses it" ;;
		*) notok "an UPDATE of the key of a table with triggers" "$out" ;;
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
		"0|"*"Gather Motion 1:1 on public.gs"*"Segment: 1"*"Remote SQL: SELECT a FROM ONLY public.gs WHERE (gp_segment_id = 1)"*)
			ok "t.gp_segment_id in a condition is sent to the one segment it names, which answers for itself" ;;
		*) notok "gp_segment_id in a sent condition" "$out / $out2" ;;
	esac
	r1=$(q 1 "SELECT count(*) FROM gr;"); r2=$(q 2 "SELECT count(*) FROM gr;")
	out=$(q 0 "SELECT count(*) FROM gr WHERE gp_segment_id = 0;")
	out2=$(q 0 "SELECT gp_segment_id, count(*) FROM gr GROUP BY 1 ORDER BY 1;" | tr '\n' ' ')
	out3=$(q 0 "SELECT gr.gp_segment_id FROM gr JOIN gs USING (a) LIMIT 1;")
	case "$out|$out2|$out3" in
		"$r1|0|$r1 1|$r2 |"*"gp_segment_id of randomly distributed table \"gr\" is known only on its segments"*)
			ok "a random table's: the segments answer a condition, the gather a query of it alone; above a join it is refused" ;;
		*) notok "gp_segment_id of a random table" "$out / $out2 (segments hold $r1 and $r2) / $out3" ;;
	esac
	out=$(q 0 "SELECT count(DISTINCT gp_segment_id) FROM gre;")
	out2=$(q 0 "SELECT gp_segment_id, count(*) FROM gp.dist_random(NULL::gre) GROUP BY 1 ORDER BY 1;")
	case "$out|$out2" in
		*'column "gp_segment_id" does not exist'*"|0|10
1|10") ok "a replicated table has none, as Cloudberry's shows none; gp.dist_random() gives each copy's" ;;
		*) notok "gp_segment_id of a replicated table" "$out / $out2" ;;
	esac
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
	# A table with a dropped column: its rows, and gp_segment_id of them,
	# the dropped column's place kept -- in a view too, which reloads.
	q 0 "CREATE TABLE gdc (a int, b text, c int) DISTRIBUTED BY (a); INSERT INTO gdc SELECT i, 'b', i * 10 FROM generate_series(1, 20) i; ALTER TABLE gdc DROP COLUMN b;" >/dev/null
	out=$(q 0 "SELECT count(*), sum(c) FROM gp.dist_random(NULL::gdc);")
	out2=$(q 0 "SELECT count(*) FROM gp_dist_random('gdc') g WHERE g.gp_segment_id <> expected_seg(g.a, 2);")
	out3=$(q 0 "CREATE VIEW gdcv AS SELECT gp_segment_id AS seg, a, c FROM gp_dist_random('gdc'); SELECT count(*), sum(c) FROM gdcv WHERE seg = expected_seg(a, 2);")
	out4=$(q 0 "SELECT pg_get_viewdef('gdcv');" | tr '\n' ' ' | sed 's/  */ /g')
	out5=$(q 0 "CREATE VIEW gdcv2 AS $(q 0 "SELECT pg_get_viewdef('gdcv');" | tr '\n' ' ' | sed 's/;[[:space:]]*$//'); SELECT count(*) FROM gdcv2;")
	[ "$out|$out2|$out3|$out5" = "20|2100|0|20|2100|20" ] \
		&& ok "gp.dist_random() and gp_segment_id of a table with a dropped column; a view of it prints and reloads" \
		|| notok "gp.dist_random() of a table with a dropped column" "$out / $out2 / $out3 / $out4 / $out5"
	# Two of them joined on gp_segment_id, which the ON clause names while
	# the join's columns are being made from theirs, as gpcheckcat's queries
	# join a catalog's rows on every segment; the dropped column's too.
	out=$(q 0 "SELECT count(*) FROM gp_dist_random('gs') x JOIN gp_dist_random('gs') y ON x.gp_segment_id = y.gp_segment_id AND x.a = y.a;")
	out2=$(q 0 "SELECT count(*), sum(x.c) FROM gp_dist_random('gdc') x JOIN gp_dist_random('gdc') y ON x.gp_segment_id = y.gp_segment_id AND x.a = y.a;")
	[ "$out|$out2" = "$(q 0 "SELECT count(*) FROM gs;")|20|2100" ] \
		&& ok "two gp_dist_random() joined on gp_segment_id, a table with a dropped column too" \
		|| notok "a join of two gp_dist_random() on gp_segment_id" "$out / $out2"
	out=$(q 0 "SELECT DISTINCT gp_segment_id FROM pg_class;")
	out2=$(q 0 "SELECT DISTINCT gp_segment_id FROM gp.dist_random(NULL::pg_namespace) ORDER BY 1;")
	[ "$out|$out2" = "-1|0
1" ] && ok "a catalog's is -1 here, and each segment's through gp.dist_random()" \
		|| notok "gp_segment_id of a catalog" "$out / $out2"
	# A query of gp_dist_random() alone that calls a function which is not
	# immutable runs on every segment, as Cloudberry runs a query over
	# gp_dist_random('gp_id') (gp_segment.c): each segment's own port, only
	# segment 1's where the condition names it, and in a subquery too.  A
	# sequence stays on the coordinator, which serves the port's; a query of
	# columns alone is the gather it was; and segment_query(), called by
	# name, runs no other kind of query.
	out=$(q 0 "SELECT gp_segment_id, current_setting('port') FROM gp_dist_random('gp_id') ORDER BY 1;")
	out2=$(q 0 "SELECT current_setting('port') FROM gp_dist_random('gp_id') WHERE gp_segment_id = 1;")
	out3=$(q 0 "SELECT count(*) FROM (SELECT current_setting('port') AS p FROM gp_dist_random('gp_id')) s WHERE p <> '$(port 0)';")
	[ "$out|$out2|$out3" = "0|$(port 1)
1|$(port 2)|$(port 2)|2" ] \
		&& ok "a query of gp_dist_random('gp_id') and a function runs on every segment, as Cloudberry runs it" \
		|| notok "a query of gp_dist_random() on the segments" "$out / $out2 / $out3"
	q 0 "CREATE SEQUENCE gdsq;" >/dev/null
	out=$(q 0 "SELECT nextval('gdsq') FROM gp_dist_random('gp_id') ORDER BY 1;" | tr '\n' ' ')
	out2=$(q 0 "EXPLAIN (COSTS OFF) SELECT * FROM gp_dist_random('gs');")
	out3=$(q 0 "EXPLAIN (COSTS OFF) SELECT pg_backend_pid() FROM gp_dist_random('gp_id');")
	out4=$(q 0 "SELECT * FROM gp_internal.segment_query('DELETE FROM gs') AS t(a int);")
	case "$out|$out2|$out3|$out4" in
		"1 2 |"*"Function Scan on dist_random"*"|"*"Function Scan on segment_query"*"|"*"runs only a query of one gp_dist_random()"*)
			ok "a sequence stays here, columns alone are gathered, and segment_query() runs nothing else" ;;
		*) notok "what stays on the coordinator" "$out / $out2 / $out3 / $out4" ;;
	esac
	# What Cloudberry evaluates here for the segments goes with the query:
	# a PL/pgSQL variable, a regclass among them, and a subquery of the
	# query's own -- the table's count, the coordinator's, where a segment
	# would count its share.  A function called for what it does gives void,
	# which travels too.
	q 0 "CREATE FUNCTION gdsq_carry(r regclass, s int) RETURNS text LANGUAGE plpgsql AS \$\$ DECLARE v text; BEGIN SELECT current_setting('port') || ' ' || pg_relation_size(r) || ' ' || (SELECT count(*) FROM gs) INTO v FROM gp_dist_random('gp_id') WHERE gp_segment_id = s; RETURN v; END \$\$;" >/dev/null
	out=$(q 0 "SELECT gdsq_carry('gs', 1);")
	out2=$(q 2 "SELECT pg_relation_size('gs');")
	out3=$(q 0 "SELECT count(*) FROM (SELECT pg_sleep(0) FROM gp_dist_random('gp_id')) s;")
	out4=$(q 0 "EXPLAIN (COSTS OFF) SELECT pg_sleep(0) FROM gp_dist_random('gp_id');")
	case "$out|$out3|$out4" in
		"$(port 2) $out2 100|2|"*"Function Scan on segment_query"*)
			ok "a variable and a subquery are evaluated here for the segments, and void comes back" ;;
		*) notok "what the coordinator evaluates for the segments" "$out ($out2) / $out3 / $out4" ;;
	esac
	# A function that runs on all segments, EXECUTE ON ALL SEGMENTS, is asked
	# of them the same way, as Cloudberry runs it: in FROM, whatever else the
	# query reads, and in the SELECT list of a query of no relation, each
	# segment calling it once; in the SELECT list of a query with FROM it is
	# refused.  Where a function runs, and what it does with SQL, are
	# pg_proc's proexeclocation and prodataaccess, as in Cloudberry, and a
	# node says which segment it is as gp_contentid.
	q 0 "CREATE FUNCTION segs_of(n int) RETURNS SETOF text LANGUAGE plpgsql
	     EXECUTE ON ALL SEGMENTS AS \$\$ BEGIN
	       RETURN NEXT current_setting('gp.contentid') || ':' || n;
	     END \$\$;" >/dev/null
	out=$(q 0 "SELECT segs_of(7) ORDER BY 1;" | tr '\n' ' ')
	out2=$(q 0 "SELECT s FROM segs_of(8) s JOIN (SELECT count(*) AS c FROM gs) g ON true ORDER BY 1;" | tr '\n' ' ')
	out3=$(q 0 "SELECT segs_of(9) FROM gs;")
	out4=$(q 0 "SELECT proexeclocation, prodataaccess FROM pg_proc WHERE proname = 'segs_of';")
	case "$out|$out2|$out3|$out4" in
		"0:7 1:7 |0:8 1:8 |"*"cannot be used in the SELECT list of a query with FROM"*"|s|n")
			ok "a function EXECUTE ON ALL SEGMENTS runs on each, in FROM and in a query of no relation, as Cloudberry runs it" ;;
		*) notok "a function EXECUTE ON ALL SEGMENTS" "$out / $out2 / $out3 / $out4" ;;
	esac
	out=$(q 0 "SELECT gp_segment_id FROM gs, gr;")
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
	out2=$(printf '%s\n' "CREATE TABLE gown (gp_segment_id int, a int) DISTRIBUTED BY (a);" \
		"CREATE TABLE gown2 (a int) DISTRIBUTED BY (a);" \
		"ALTER TABLE gown2 ADD COLUMN gp_segment_id int;" \
		"ALTER TABLE gown2 RENAME COLUMN a TO gp_segment_id;" \
		"CREATE VIEW gownv AS SELECT 7 AS gp_segment_id;" \
		"SELECT gp_segment_id FROM gownv;" | qf 0 | grep -c 'column name "gp_segment_id" conflicts with a system column name')
	out3=$(q 0 "SELECT gp_segment_id FROM gownv;")
	[ "$out|$out2|$out3" = "0|3|7" ] \
		&& ok "a segment's utility session answers its own id; a table's column may not be called so, as Cloudberry refuses a system column's name, and a view's may" \
		|| notok "gp_segment_id on a segment, and a column of that name" "$out / $out2 / $out3"

	# WHERE CURRENT OF a cursor of a distributed table: its gather says which
	# segment the current row came from, and its ctid there.
	q 0 "CREATE TABLE cur (a int, b text) DISTRIBUTED BY (a); INSERT INTO cur SELECT g, 'c' FROM generate_series(1, 20) g;" >/dev/null
	out=$(printf '%s\n' "BEGIN;" "DECLARE c1 CURSOR FOR SELECT a FROM cur WHERE a IN (4, 5);" \
		"FETCH 1 FROM c1;" "UPDATE cur SET b = 'updated' WHERE CURRENT OF c1;" \
		"FETCH 1 FROM c1;" "DELETE FROM cur WHERE CURRENT OF c1;" "COMMIT;" \
		"SELECT count(*), count(*) FILTER (WHERE b = 'updated') FROM cur;" | qf 0 | tr '\n' ' ')
	out2=$(printf '%s\n' "BEGIN;" "DECLARE c2 CURSOR FOR SELECT a FROM cur ORDER BY a;" "FETCH 1 FROM c2;" \
		"UPDATE cur SET b = 'x' WHERE CURRENT OF c2;" "ROLLBACK;" | qf 0 2>&1)
	out3=$(printf '%s\n' "BEGIN;" "DECLARE c3 CURSOR FOR SELECT k FROM rep;" "FETCH 1 FROM c3;" \
		"UPDATE rep SET v = 'x' WHERE CURRENT OF c3;" "ROLLBACK;" | qf 0 2>&1)
	case "$out|$out2|$out3" in
		*"19|1 |"*'cursor "c2" is not a simply updatable scan of table "cur"'*"|"*'"rep" is not simply updatable'*)
			ok "UPDATE and DELETE WHERE CURRENT OF change the cursor's row where it is; a sorted cursor and a replicated table are refused, as in Cloudberry" ;;
		*) notok "WHERE CURRENT OF" "$out / $out2 / $out3" ;;
	esac

	# A cursor without FOR UPDATE leaves its row free: another transaction
	# updates it after the FETCH, and WHERE CURRENT OF finds its new version,
	# as PostgreSQL's TID scan follows the row's updates -- the old ctid alone
	# finds the version the update left, which the statement no longer sees.
	for stmt in "UPDATE cur SET b = b || '+cur' WHERE CURRENT OF c4" "DELETE FROM cur WHERE CURRENT OF c4"; do
		printf '%s\n' "BEGIN;" "DECLARE c4 CURSOR FOR SELECT a FROM cur WHERE a = 7;" "FETCH 1 FROM c4;" \
			"SELECT pg_sleep(2);" "$stmt;" "COMMIT;" | qf 0 > "$ROOT/cur4.out" 2>&1 &
		holder=$!
		sleep 0.7
		q 0 "UPDATE cur SET b = b || '+other' WHERE a = 7;" >/dev/null
		wait "$holder"
		out=$(q 0 "SELECT coalesce(string_agg(b, ','), 'none') FROM cur WHERE a = 7;")
		case "$stmt|$out" in
			UPDATE*"|c+other+cur"|DELETE*"|none")
				ok "${stmt%% *} WHERE CURRENT OF finds the cursor's row in the version another transaction updated since the FETCH" ;;
			*) notok "${stmt%% *} WHERE CURRENT OF after a concurrent update" "$(tr '\n' ' ' < "$ROOT/cur4.out") / $out" ;;
		esac
	done

	# A partial table: its rows on the first so many segments, as Cloudberry's
	# gp_debug_numsegments makes one (gp_sql's distribution.c), and read,
	# written and counted there alone.
	q 0 "CREATE EXTENSION gp_debug_numsegments;" >/dev/null
	out=$(printf '%s\n' "SELECT gp_debug_set_create_table_default_numsegments(1);" \
		"CREATE TABLE pt1 (a int, b int) DISTRIBUTED BY (a);" \
		"CREATE TABLE pr1 (a int, b int) DISTRIBUTED REPLICATED;" \
		"CREATE TABLE pn1 (a int, b int) DISTRIBUTED RANDOMLY;" \
		"CREATE TABLE pp1 (a int, b int) DISTRIBUTED BY (a) PARTITION BY RANGE (b) (START (1) END (3) EVERY (1));" \
		"CREATE TABLE pc1 AS SELECT g AS a FROM generate_series(1, 20) g DISTRIBUTED BY (a);" \
		"SELECT gp_debug_reset_create_table_default_numsegments();" \
		"CREATE TABLE pf1 (a int, b int) DISTRIBUTED BY (a);" \
		"SELECT gp_debug_get_create_table_default_numsegments();" \
		"SELECT string_agg(c.relname || ':' || (gp.policy(c.oid)).numsegments, ' ' ORDER BY c.relname) FROM pg_class c WHERE c.relname IN ('pt1', 'pr1', 'pn1', 'pc1', 'pf1') OR c.relname LIKE 'pp1_1_prt_%';" | qf 0 | tr '\n' '/')
	[ "$out" = "1//FULL/pc1:1 pf1:2 pn1:1 pp1_1_prt_1:1 pp1_1_prt_2:1 pr1:1 pt1:1/" ] \
		&& ok "gp_debug_numsegments spreads the tables made after it over one segment, partitions and CREATE TABLE AS too" \
		|| notok "gp_debug_set_create_table_default_numsegments()" "$out"

	q 0 "INSERT INTO pt1 SELECT g, g FROM generate_series(1, 100) g; INSERT INTO pr1 SELECT g, g FROM generate_series(1, 10) g; INSERT INTO pn1 SELECT g, g FROM generate_series(1, 50) g;" >/dev/null
	count_p1="SELECT (SELECT count(*) FROM pt1) || ' ' || (SELECT count(*) FROM pr1) || ' ' || (SELECT count(*) FROM pn1) || ' ' || (SELECT count(*) FROM pc1);"
	w1=$(q 1 "$count_p1"); w2=$(q 2 "$count_p1")
	out=""
	for i in 1 2 3 4; do out="$out$(q 0 "$count_p1")/"; done
	[ "$w1|$w2|$out" = "100 10 50 20|0 0 0 0|100 10 50 20/100 10 50 20/100 10 50 20/100 10 50 20/" ] \
		&& ok "their rows are on segment 0 alone, and every session reads them there, a replicated one's too" \
		|| notok "where a partial table's rows are" "segment 0: $w1, segment 1: $w2, coordinator: $out"

	out=$(q 0 "EXPLAIN (COSTS OFF) SELECT * FROM pt1;")
	out2=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" "SELECT count(*) FROM pn1;" \
		"UPDATE pr1 SET b = b + 1;" "SELECT b FROM pt1 WHERE a = 7;" | qf 0)
	info=$(printf '%s\n' "$out2" | grep -o 'INFO:  (slice.*' | tr '\n' '/')
	rows=$(printf '%s\n' "$out2" | grep -v 'INFO:' | tr '\n' '/')
	case "$out|$info|$rows" in
		*"Gather Motion 1:1 on pt1"*"(slice1; segments: 1)"*"|INFO:  (slice 1) Dispatch command to SINGLE content/INFO:  (slice 0) Dispatch command to SINGLE content/INFO:  (slice 1) Dispatch command to SINGLE content/|50/7/")
			ok "a gather, a write and direct dispatch ask its one segment, and say so" ;;
		*) notok "dispatch to a partial table's segments" "$out / $info / $rows" ;;
	esac

	q 0 "ALTER TABLE pt1 SET DISTRIBUTED BY (b);" >/dev/null
	out=$(q 0 "SELECT numsegments FROM gp.policy('pt1');")
	w1=$(q 1 "SELECT count(*) FROM pt1;"); w2=$(q 2 "SELECT count(*) FROM pt1;")
	[ "$out|$w1|$w2" = "1|100|0" ] \
		&& ok "ALTER TABLE ... SET DISTRIBUTED keeps it on its segments" \
		|| notok "a partial table redistributed" "$out / $w1 $w2"

	# ALTER TABLE ... EXPAND TABLE, Cloudberry's way out of a partial table:
	# spread over every segment, its rows moved, no trigger fired -- this
	# one would drop every row; SHRINK TABLE TO n back (distribution.c).
	out=$(printf '%s\n' "SELECT gp_debug_set_create_table_default_numsegments(1);" \
		"CREATE TABLE px (a int, b int) DISTRIBUTED BY (a);" \
		"SELECT gp_debug_reset_create_table_default_numsegments();" \
		"INSERT INTO px SELECT i, i FROM generate_series(1, 100) i;" \
		"CREATE FUNCTION px_drop() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RETURN NULL; END \$\$;" \
		"CREATE TRIGGER px_bi BEFORE INSERT ON px FOR EACH ROW EXECUTE FUNCTION px_drop();" \
		"ALTER TABLE px EXPAND TABLE;" \
		"SELECT (gp.policy('px')).numsegments || ':' || count(*) FROM px;" | qf 0 | tail -1)
	w1=$(q 1 "SELECT count(*) FROM px WHERE expected_seg(a, 2) <> 0;"); w2=$(q 2 "SELECT count(*) FROM px WHERE expected_seg(a, 2) <> 1;")
	s2=$(q 2 "SELECT count(*) FROM px;")
	out2=$(q 0 "ALTER TABLE px EXPAND TABLE;")
	out3=$(q 0 "ALTER TABLE px SHRINK TABLE TO 1; SELECT (gp.policy('px')).numsegments || ':' || count(*) FROM px; SELECT tgenabled FROM pg_trigger WHERE tgname = 'px_bi';" | tr '\n' ' ')
	s2b=$(q 2 "SELECT count(*) FROM px;")
	case "$out|$w1|$w2|$out2|$out3|$s2b" in
		"2:100|0|0|"*'cannot expand table "px"'*"table has already been expanded"*"|1:100 O |0")
			isnum "$s2" && [ "$s2" -gt 0 ] \
				&& ok "EXPAND TABLE spreads a partial table over every segment, no trigger fired; again, refused; SHRINK TABLE TO n back" \
				|| notok "EXPAND TABLE: segment 1's rows" "$s2" ;;
		*) notok "EXPAND TABLE and SHRINK TABLE" "$out / misplaced $w1 $w2 / $out2 / $out3 / $s2b" ;;
	esac

	# gp_distribution_policy: the labels in Cloudberry's catalog's columns,
	# and written through it, as that is (gp_catalog.c).
	int4_ops=$(q 0 "SELECT c.oid FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod WHERE a.amname = 'hash' AND c.opcname = 'int4_ops';")
	out=$(q 0 "SELECT format('%s %s %s %s %s', localoid::regclass, policytype, numsegments, distkey, distclass) FROM gp_distribution_policy WHERE localoid IN ('d'::regclass, 'pt1'::regclass, 'pr1'::regclass, 'pn1'::regclass) ORDER BY 1;" | tr '\n' '/')
	[ "$out" = "d p 2 1 $int4_ops/pn1 p 1  /pr1 r 1  /pt1 p 1 2 $int4_ops/" ] \
		&& ok "gp_distribution_policy shows each table's policy as Cloudberry's catalog does" \
		|| notok "gp_distribution_policy" "$out"

	q 0 "CREATE TABLE dp (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	out=$(q 0 "DELETE FROM gp_distribution_policy WHERE localoid = 0;")
	out2=$(printf '%s\n' "SET allow_system_table_mods = on;" \
		"UPDATE gp_distribution_policy SET numsegments = 1 WHERE localoid = 'dp'::regclass;" \
		"SELECT numsegments FROM gp.policy('dp');" \
		"UPDATE gp_distribution_policy SET distkey = '', distclass = '' WHERE localoid = 'dp'::regclass;" \
		"SELECT kind FROM gp.policy('dp');" \
		"DELETE FROM gp_distribution_policy WHERE localoid = 'dp'::regclass;" \
		"SELECT count(*) FROM gp_distribution_policy WHERE localoid = 'dp'::regclass;" \
		"INSERT INTO dp VALUES (1, 1), (2, 2);" | qf 0 | tr '\n' '/')
	w0=$(q 0 "SELECT count(*) FROM dp;"); w1=$(q 1 "SELECT count(*) FROM dp;")
	p1=$(q 1 "SELECT count(*) FROM gp_distribution_policy WHERE localoid = 'dp'::regclass;")
	case "$out|$out2|$w0 $w1 $p1" in
		*'permission denied: "gp_distribution_policy" is a system catalog'*"|1/random/0/|2 0 0")
			ok "a write to it, with allow_system_table_mods, is the policy's, on every node; a deleted one leaves the table to the coordinator" ;;
		*) notok "writes to gp_distribution_policy" "$out / $out2 / $w0 $w1 $p1" ;;
	esac

	q 0 "SET allow_system_table_mods = on; INSERT INTO gp_distribution_policy SELECT 'dp'::regclass, 'p', 5, '1', '';" >/dev/null
	out=$(q 0 "SELECT numsegments FROM gp_distribution_policy WHERE localoid = 'dp'::regclass;")
	out2=$(q 0 "SELECT count(*) FROM dp;" | tr '\n' '/')
	out3=$(q 0 "SET allow_system_table_mods = on; UPDATE gp_distribution_policy SET numsegments = 2, distclass = (SELECT c.oid FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod WHERE a.amname = 'hash' AND c.opcname = 'int8_ops')::text::oidvector WHERE localoid = 'dp'::regclass;")
	case "$out|$out2|$out3" in
		"5|ERROR:  cannot access table \"dp\" in current transaction/DETAIL:  Its distribution policy spreads it over 5 segments, and the cluster has 2./|"*'does not hash column "a" of type integer'*)
			ok "a policy of more segments than the cluster has is refused where the table is read; an operator class not of the column's type, where it is written" ;;
		*) notok "gp_distribution_policy's refusals" "$out / $out2 / $out3" ;;
	esac

	# The label names the key's columns, where Cloudberry's catalog numbers
	# them: a column renamed is renamed in it, and one dropped from the key
	# leaves the table random, as Cloudberry leaves it.
	q 0 "CREATE TABLE kc (a int, b int, c int) DISTRIBUTED BY (a, b) PARTITION BY RANGE (c) (START (1) END (3) EVERY (1));" >/dev/null
	q 0 "INSERT INTO kc SELECT g, g, 1 + g % 2 FROM generate_series(1, 20) g;" >/dev/null
	q 0 "ALTER TABLE kc RENAME COLUMN a TO \"A a\";" >/dev/null
	kc="SELECT string_agg(gp_sql.distribution(c.oid), ' ' ORDER BY c.relname) FROM pg_class c WHERE c.relname LIKE 'kc%' AND c.relkind IN ('r', 'p');"
	out=$(q 0 "$kc"); out2=$(q 1 "$kc")
	out3=$(q 0 "ALTER TABLE kc DROP COLUMN b;")
	out4=$(q 0 "$kc"); out5=$(q 2 "$kc"); out6=$(q 0 "SELECT count(*), sum(\"A a\") FROM kc;")
	n=$(printf '%s\n' "$out3" | grep -c "dropping a column that is part of the distribution policy forces a random distribution policy")
	[ "$out|$out2|$n|$out4|$out5|$out6" = '("A a",b) ("A a",b) ("A a",b)|("A a",b) ("A a",b) ("A a",b)|3|random random random|random random random|20|210' ] \
		&& ok "a key column renamed is renamed in the policy; one dropped leaves the table random, with Cloudberry's NOTICE" \
		|| notok "the key's columns renamed and dropped" "$out / $out2 / $out3 / $out4 / $out5 / $out6"

	# A randomly distributed table's first rows of a statement are dealt one
	# to each segment in turn, from one chosen at random: a statement of as
	# many rows as segments reaches every one (direct_dispatch's ten rows,
	# which a random choice for each row left a segment of one statement in
	# nineteen); the rows after them go to a segment chosen at random, as
	# Cloudberry's do, so that two layouts of a table differ.
	q 0 "CREATE TABLE rrt (a int) DISTRIBUTED RANDOMLY;" >/dev/null
	out=""
	for i in 1 2 3; do
		out="$out$(q 0 "TRUNCATE rrt; INSERT INTO rrt SELECT generate_series(1, 2);
				   SELECT string_agg(n::text, ' ') FROM (SELECT count(*) AS n FROM rrt
				   GROUP BY gp_segment_id ORDER BY gp_segment_id) c;") "
	done
	out2=$(q 0 "TRUNCATE rrt; INSERT INTO rrt SELECT generate_series(1, 1000);
				SELECT count(*) FILTER (WHERE n BETWEEN 400 AND 600), count(*)
				FROM (SELECT count(*) AS n FROM rrt GROUP BY gp_segment_id) c;")
	[ "$out|$out2" = "1 1 1 1 1 1 |2|2" ] && ok "a random table's first rows are dealt one to each segment, the rest at random ($out)" \
		|| notok "the rows of a randomly distributed table" "$out / $out2"

	# A gather the plan reads again -- the inner side of a Nested Loop --
	# keeps the rows it read, and the segments run its query once.
	q 0 "CREATE TABLE nlo (a int) DISTRIBUTED BY (a); INSERT INTO nlo SELECT generate_series(1, 6);
		 CREATE TABLE nli (a int, t text) DISTRIBUTED BY (a); INSERT INTO nli SELECT i, md5(i::text) FROM generate_series(1, 50) i;" >/dev/null
	scans() { q 1 "SELECT seq_scan FROM pg_stat_user_tables WHERE relname = 'nli';"; }
	before=$(scans)
	out=$(q 0 "SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;
			   SELECT count(*) FROM nlo, nli WHERE nlo.a = nli.a;")
	after=$before
	for i in $(seq 1 40); do
		after=$(scans)
		[ "$after" != "$before" ] && break
		sleep 0.25
	done
	isnum "$before" && isnum "$after" && [ "$out" = "6" ] && [ $((after - before)) -eq 1 ] \
		&& ok "a Nested Loop's inner gather keeps its rows: the segment scanned the table once for six outer rows" \
		|| notok "a rescanned gather" "count $out, scans $before -> $after"
	out=$(q 0 "SELECT count(*) FROM (SELECT 1) s, (SELECT count(*) AS n FROM nli) l WHERE l.n > 0;")
	[ "$out" = "1" ] && ok "and one that reads no column keeps rows of NULLs (a segfault once)" \
		|| notok "a kept gather of count(*)" "$out"

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

	# VACUUM ANALYZE: the coordinator runs the statement before it dispatches
	# it whole, and its ANALYZE samples the segments all the same -- once
	# sampled the coordinator's empty copy, and left no statistics at all.
	q 0 "CREATE TABLE vst (a int, g int) DISTRIBUTED BY (a); INSERT INTO vst SELECT i, i % 10 FROM generate_series(1, 2000) i;" >/dev/null
	q 0 "VACUUM ANALYZE vst;" >/dev/null
	out=$(q 0 "SELECT n_distinct FROM pg_stats WHERE tablename = 'vst' AND attname = 'g';")
	q 0 "VACUUM (FULL, ANALYZE) vst;" >/dev/null
	out2=$(q 0 "SELECT n_distinct FROM pg_stats WHERE tablename = 'vst' AND attname = 'g';")
	[ "$out|$out2" = "10|10" ] && ok "VACUUM ANALYZE, and VACUUM FULL's, sample the segments too" \
		|| notok "the statistics of VACUUM ANALYZE" "$out / $out2"

	q 0 "ANALYZE sales;" >/dev/null
	out=$(q 0 "SELECT reltuples FROM pg_class WHERE relname = 'sales';")
	out2=$(q 0 "SELECT count(*) FROM pg_stats WHERE tablename = 'sales' AND inherited;")
	[ "$out" = "90" ] && isnum "$out2" && [ "$out2" -eq 3 ] \
		&& ok "a partitioned table is analyzed through its partitions" \
		|| notok "ANALYZE of a partitioned table" "$out / inherited stats: $out2"

	# ANALYZE of a partitioned table as Cloudberry's (gp_partanalyze.c,
	# gp_partmerge.c): its leaves, and then the root, whose statistics are
	# its leaves' merged -- the histograms merged bucket by bucket, the
	# number of distinct values from each leaf's HyperLogLog counter, no
	# correlation -- as Cloudberry's are; a partitioned table under another
	# refused while its setting is off; and FULLSCAN, whose leaves count the
	# distinct values of every row.
	q 0 "CREATE TABLE mrg (a int, b int, c int) DISTRIBUTED BY (a) PARTITION BY RANGE (a);
	     CREATE TABLE mrg1 PARTITION OF mrg FOR VALUES FROM (0) TO (10);
	     CREATE TABLE mrg2 PARTITION OF mrg FOR VALUES FROM (10) TO (20);
	     CREATE TABLE mrg3 PARTITION OF mrg FOR VALUES FROM (20) TO (30);
	     INSERT INTO mrg SELECT i, i % 4, i % 2 FROM generate_series(0, 19) i; ANALYZE mrg;" >/dev/null
	out=$(q 0 "SELECT histogram_bounds || ' ' || coalesce(correlation::text, 'none') || ' ' || n_distinct
	             FROM pg_stats WHERE tablename = 'mrg' AND attname = 'a';")
	out2=$(q 0 "SELECT reltuples || ' ' || relpages FROM pg_class WHERE relname IN ('mrg', 'mrg3') ORDER BY relname;" | tr '\n' ' ')
	[ "$out" = "{0,1,2,3,4,5,6,7,8,9,11,12,13,14,15,16,17,18,19} none -1" ] && [ "$out2" = "20 -1 0 1 " ] \
		&& ok "the root's statistics are its leaves' merged, and an empty leaf analyzed has a page" \
		|| notok "the merge of a root's statistics" "$out / $out2"
	out=$(q 0 "SELECT n_distinct || ' ' || most_common_vals::text FROM pg_stats WHERE tablename = 'mrg' AND attname = 'c';")
	out2=$(q 0 "SELECT count(*) FROM gp_internal.leaf_hll WHERE starelid = 'mrg1'::regclass;")
	[ "$out" = "2 {0,1}" ] && [ "$out2" = "3" ] \
		&& ok "the most common values merged, from each leaf's counters of its columns" \
		|| notok "the merged most common values" "$out / counters: $out2"
	q 0 "CREATE TABLE mrg4 PARTITION OF mrg FOR VALUES FROM (30) TO (40) PARTITION BY LIST (b);
	     CREATE TABLE mrg41 PARTITION OF mrg4 FOR VALUES IN (1);" >/dev/null
	out=$(q 0 "ANALYZE mrg4;" 2>&1)
	out2=$(q 0 "SET gp.optimizer_analyze_midlevel_partition = on; ANALYZE mrg4;
	            SELECT relpages FROM pg_class WHERE relname = 'mrg4';" 2>&1)
	case "$out|$out2" in
		*"cannot analyze a mid-level partition"*"|-1")
			ok "a mid-level partitioned table is refused, unless its setting says to analyze it" ;;
		*) notok "ANALYZE of a mid-level partitioned table" "$out / $out2" ;;
	esac
	q 0 "INSERT INTO mrg SELECT i % 20, i, i % 50 FROM generate_series(1, 1000) i; ANALYZE FULLSCAN mrg;" >/dev/null
	out=$(q 0 "SELECT n_distinct FROM pg_stats WHERE tablename = 'mrg1' AND attname = 'c';")
	out2=$(q 0 "SELECT count(*) = 1 FROM gp_internal.leaf_hll WHERE starelid = 'mrg1'::regclass AND staattnum = 3 AND fullscan;")
	out3=$(q 0 "SELECT round(gp_hyperloglog_get_estimate(gp_hyperloglog_accum(c))) FROM mrg;")
	[ "$out" = "50" ] && [ "$out2" = "t" ] && [ "$out3" = "50" ] \
		&& ok "ANALYZE FULLSCAN counts a leaf's distinct values over every row, with gp_hyperloglog_accum()" \
		|| notok "ANALYZE FULLSCAN" "$out / $out2 / $out3"

	# The coordinator's own VACUUM of its empty copy counts nothing, and its
	# own ANALYZE nothing all-visible; Cloudberry's bring back the segments'
	# counts, and so do these.
	q 0 "CREATE INDEX st_a ON st (a);" >/dev/null
	q 0 "VACUUM st;" >/dev/null
	out=$(q 0 "SELECT relpages || ' ' || reltuples || ' ' || relallvisible
	             FROM pg_class WHERE relname = 'st';")
	out2=$(q 0 "SELECT sum(relpages) || ' ' || sum(reltuples) || ' ' || sum(relallvisible)
	              FROM gp_dist_random('pg_class') WHERE relname = 'st';")
	[ "$out" = "$out2" ] && [ "${out%% *}" != "0" ] \
		&& ok "VACUUM brings back the segments' pages, rows and all-visible pages" \
		|| notok "VACUUM of a distributed table's counts" "$out, segments: $out2"
	out=$(q 0 "SELECT relpages || ' ' || reltuples FROM pg_class WHERE relname = 'st_a';")
	out2=$(q 0 "SELECT sum(relpages) || ' ' || sum(reltuples)
	              FROM gp_dist_random('pg_class') WHERE relname = 'st_a';")
	[ "$out" = "$out2" ] && [ "${out%% *}" != "0" ] \
		&& ok "and its indexes' pages and rows" \
		|| notok "VACUUM of an index's counts" "$out, segments: $out2"
	q 0 "ANALYZE st;" >/dev/null
	out=$(q 0 "SELECT relallvisible FROM pg_class WHERE relname = 'st';")
	out2=$(q 0 "SELECT sum(relallvisible) FROM gp_dist_random('pg_class') WHERE relname = 'st';")
	[ "$out" = "$out2" ] && [ "$out" != "0" ] \
		&& ok "ANALYZE keeps the segments' all-visible pages, where its own count is none" \
		|| notok "ANALYZE of the all-visible pages" "$out, segments: $out2"
	# An index build on the coordinator counts its empty copy, and writes
	# what it counts (index_update_stats()), which Cloudberry's coordinator
	# never writes: the counts ANALYZE brought back stay, through CREATE
	# INDEX, REINDEX and ALTER TABLE ... ADD UNIQUE.
	before=$(q 0 "SELECT relpages || ' ' || reltuples FROM pg_class WHERE relname = 'st';")
	q 0 "CREATE INDEX st_ag ON st (a, g); REINDEX TABLE st; ALTER TABLE st ADD CONSTRAINT st_u UNIQUE (a, g);" >/dev/null 2>&1
	out=$(q 0 "SELECT relpages || ' ' || reltuples FROM pg_class WHERE relname = 'st';")
	q 0 "ALTER TABLE st DROP CONSTRAINT st_u; DROP INDEX st_ag;" >/dev/null 2>&1
	[ "$out" = "$before" ] && [ "${out%% *}" != "0" ] \
		&& ok "... and an index build keeps them, where the coordinator's would count its empty copy" \
		|| notok "a table's counts after an index build" "$out, before: $before"
	q 0 "VACUUM rst;" >/dev/null
	out=$(q 0 "SELECT relpages || ' ' || reltuples FROM pg_class WHERE relname = 'rst';")
	out2=$(q 1 "SELECT relpages || ' ' || reltuples FROM pg_class WHERE relname = 'rst';")
	[ "$out" = "$out2" ] && [ "$out" != "0 0" ] \
		&& ok "a replicated table's are one segment's worth" \
		|| notok "VACUUM of a replicated table" "$out, one segment: $out2"

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

	# A statistics row written by hand, as ORCA's tests, gpsd and minirepro
	# write pg_statistic: an array constant for a column of type anyarray is
	# taken as anyarray, as Cloudberry's parser takes it, in a VALUES list of
	# several rows too; and values not of their column's type are refused as
	# the planner reads them, in Cloudberry's words, where PostgreSQL's
	# planner would crash comparing them.
	q 0 "CREATE TABLE hst (a int, b text) DISTRIBUTED BY (a);
		 INSERT INTO hst SELECT i, 'v' || (i % 5) FROM generate_series(1, 100) i; ANALYZE hst;" >/dev/null
	out=$(qf 0 <<'EOF'
SET allow_system_table_mods = on;
DELETE FROM pg_statistic WHERE starelid = 'hst'::regclass;
INSERT INTO pg_statistic VALUES
 ('hst'::regclass, 1, false, 0, 4, -1, 1, 0, 0, 0, 0, 96, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  '{0.5}'::real[], NULL, NULL, NULL, NULL, '{7}'::int[], NULL, NULL, NULL, NULL),
 ('hst'::regclass, 2, false, 0, 3, -0.05, 1, 0, 0, 0, 0, 98, 0, 0, 0, 0, 100, 0, 0, 0, 0,
  '{0.9}'::real[], NULL, NULL, NULL, NULL, '{v1}'::text[], NULL, NULL, NULL, NULL);
SELECT string_agg(most_common_vals::text, ' ' ORDER BY attname) FROM pg_stats WHERE tablename = 'hst';
EOF
)
	est=$(q 0 "EXPLAIN SELECT * FROM hst WHERE b = 'v1';" | sed -n 's/.*rows=\([0-9]*\).*/\1/p' | head -1)
	[ "$out" = "{7} {v1}" ] && [ "$est" = "90" ] \
		&& ok "a statistics row written by hand, its arrays taken as anyarray, is the planner's ($est rows)" \
		|| notok "pg_statistic written by hand" "$out / $est"
	out=$(q 0 "SET allow_system_table_mods = on;
			   UPDATE pg_statistic SET stavalues1 = '{1,2}'::int[] WHERE starelid = 'hst'::regclass AND staattnum = 2;
			   RESET allow_system_table_mods;
			   SELECT count(*) FROM hst WHERE b = 'v1';")
	case "$out" in
		*"invalid MCV array of type integer, for attribute of type text"*)
			ok "an MCV list not of its column's type is refused as Cloudberry refuses it" ;;
		*) notok "statistics not of their column's type" "$out" ;;
	esac
	q 0 "ANALYZE hst;" >/dev/null

	# What ANALYZE samples of a segment is that segment's count too, as
	# Cloudberry's gp_acquire_sample_rows() writes it there: the table's
	# pages and rows, and its index's.  So a VACUUM whose segments scan a
	# page or none -- a read made the rest all-visible, as PostgreSQL 19's
	# pruning does -- keeps the rows ANALYZE counted, where the coordinator
	# once read 0 of them until the next ANALYZE; and an index's pages after
	# ANALYZE are its files' on the segments, not the empty copy's page.  The
	# rows are written by the explicit write (RETURNING), a statement of
	# VALUES a batch, which leaves no page empty behind them: COPY's, a bulk
	# insert, extends a table by pages it leaves empty at its end, which a
	# VACUUM scans, and PostgreSQL 19 takes for as full as the pages it did
	# not scan (vac_estimate_reltuples()), on one node as here.
	q 0 "CREATE TABLE sgc (a int, b int) DISTRIBUTED BY (a); CREATE INDEX sgc_b ON sgc (b);
		 INSERT INTO sgc SELECT i, i FROM generate_series(1, 20010) i RETURNING 0; ANALYZE sgc;" >/dev/null
	out=$(q 0 "SELECT relpages || ' ' || reltuples FROM pg_class WHERE relname = 'sgc';")
	out2=$(q 0 "SELECT sum(relpages) || ' ' || sum(reltuples) FROM gp_dist_random('pg_class') WHERE relname = 'sgc';")
	out3=$(q 0 "SELECT relpages || ' ' || reltuples FROM pg_class WHERE relname = 'sgc_b';")
	seg=0
	for n in 1 2; do
		p=$(q "$n" "SELECT pg_relation_size('sgc_b') / current_setting('block_size')::int;")
		isnum "$p" && seg=$((seg + p))
	done
	[ "$out" = "$out2" ] && [ "${out#* }" = "20010" ] && [ "$out3" = "$seg 20010" ] \
		&& ok "the segments count what ANALYZE samples of them, and an index's pages are the segments' ($seg)" \
		|| notok "the counts ANALYZE leaves" "$out / segments: $out2 / index: $out3, $seg pages"
	q 0 "SELECT count(*) FROM sgc;" >/dev/null
	q 0 "VACUUM sgc;" >/dev/null
	out=$(q 0 "SELECT reltuples FROM pg_class WHERE relname = 'sgc';")
	out2=$(q 0 "SELECT reltuples FROM pg_class WHERE relname = 'sgc_b';")
	[ "$out|$out2" = "20010|20010" ] && ok "... which a VACUUM after a read keeps, the table's and the index's" \
		|| notok "the rows after a VACUUM that scanned little" "$out / index: $out2"

	# A replicated table's ANALYZE samples one of its segments, and a VACUUM
	# of the others, read there, counts none: they hold as many rows to a page
	# as the one that counted, where they once made the table a third of it.
	q 0 "CREATE TABLE sgr (a int) DISTRIBUTED REPLICATED; INSERT INTO sgr SELECT generate_series(1, 3000) RETURNING 0; ANALYZE sgr;" >/dev/null
	for n in 1 2; do q "$n" "SELECT count(*) FROM sgr;" >/dev/null; done
	q 0 "VACUUM sgr;" >/dev/null
	out=$(q 0 "SELECT reltuples FROM pg_class WHERE relname = 'sgr';")
	[ "$out" = "3000" ] && ok "a replicated table's rows after a VACUUM of segments that had not counted them" \
		|| notok "a replicated table's rows after VACUUM" "$out"

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

	# An aggregate called with ORDER BY takes its rows in that order: not a
	# partial aggregate on each segment and a final one combining them,
	# which is each segment's rows in order, one segment after another.
	# ORCA aggregates the gathered rows, as the planner does; one without
	# ORDER BY is still split.
	orca_same "an aggregate called with ORDER BY: the gathered rows, aggregated in its order" \
		"SELECT string_agg(c, ',' ORDER BY a), array_agg(a ORDER BY a DESC) FROM o WHERE a < 40;" \
		"Aggregate"
	plan=$(q 0 "EXPLAIN (COSTS OFF) SELECT string_agg(c, ',' ORDER BY a) FROM o;")
	plan2=$(q 0 "EXPLAIN (COSTS OFF) SELECT string_agg(c, ',') FROM o;")
	[[ "$plan" != *"Partial Aggregate"* && "$plan2" == *"Partial Aggregate"* ]] \
		&& ok "... not split into a partial aggregate on each segment, as one without ORDER BY is" \
		|| notok "an aggregate called with ORDER BY, split" "$plan / $plan2"
	# ORCA decides what it may split by the aggregate's metadata, and the
	# call is given it under an id of its own: the aggregation that calls it
	# is not split, and another of the same query still is.
	plan=$(q 0 "EXPLAIN (COSTS OFF) SELECT (SELECT string_agg(c, ',' ORDER BY a) FROM o WHERE a < 20), (SELECT sum(a) FROM o);")
	[[ "$plan" == *"Aggregate"*"Gather Motion"*"Finalize Aggregate"*"Partial Aggregate"* ]] \
		&& [[ "$plan" != *"Partial Aggregate"*"Partial Aggregate"* ]] \
		&& ok "... and beside it another aggregation of the query is split, as the planner's would be" \
		|| notok "an ordered call beside another aggregation" "$plan"
	orca_same "... and answers so" \
		"SELECT (SELECT string_agg(c, ',' ORDER BY a) FROM o WHERE a < 20), (SELECT sum(a) FROM o);"
	orca_same "one key's rows: direct dispatch to its segment" \
		"SELECT * FROM o WHERE a = 42;" "Gather Motion 1:1  (slice1; segments: 1)"

	# A Gather Motion's rows reach a client that asked for another encoding
	# than the database's in its own, as the planner's gathers do (section 8).
	out=$(printf '%s\n' "SET client_encoding = 'LATIN1';" "SELECT t FROM enc ORDER BY a;" \
		| qf 0 | od -An -tx1 | tr -d ' \n')
	want=$(printf 'funny char \304\ncopied \304\n' | od -An -tx1 | tr -d ' \n')
	plan=$(q 0 "EXPLAIN (COSTS OFF) SELECT t FROM enc ORDER BY a;")
	[ "$out" = "$want" ] && [[ "$plan" == *"Gather Motion"*"Optimizer: GPORCA"* ]] \
		&& ok "a client's own encoding for a segment's text, under ORCA" \
		|| notok "a client encoding not the database's, under ORCA" "$out / $plan"

	# now() is the transaction's start, and in a segment's slice ORCA's plan
	# evaluates it there: each segment's process took its own, none of them
	# the coordinator's.  A fragment is sent with the coordinator's
	# transaction and statement start times, and the segment takes them, as
	# Cloudberry's do.  A second's sleep first, so that a segment's own
	# would differ.
	out=$(q 0 "BEGIN; SELECT pg_sleep(1.1);
		SELECT count(DISTINCT x) || ' ' || bool_and(x = now())
			FROM (SELECT now() AS x FROM o) s;
		SELECT count(DISTINCT x) || ' ' || bool_and(x = statement_timestamp())
			FROM (SELECT statement_timestamp() AS x FROM o) s;
		SELECT count(DISTINCT x) || ' ' || bool_and(x = localtimestamp(3))
			FROM (SELECT localtimestamp(3) AS x FROM o) s;
		COMMIT;" | grep -v '^$' | tr '\n' ' ')
	plan=$(q 0 "EXPLAIN (COSTS OFF) SELECT count(DISTINCT x) FROM (SELECT localtimestamp(3) AS x FROM o) s;")
	[ "$out" = "1 true 1 true 1 true " ] && [[ "$plan" == *"Redistribute Motion"*"Optimizer: GPORCA"* ]] \
		&& ok "now(), statement_timestamp() and LOCALTIMESTAMP on the segments are the coordinator's" \
		|| notok "now() on the segments" "$out / $plan"

	# A sequence is the coordinator's: nextval() in a slice the segments run
	# takes the coordinator's sequence's values, a segment asking the
	# coordinator for a block of its CACHE at a time (gp_core's gp_seq.c) --
	# a serial column's default, an identity column's next value, which a
	# role may take without a privilege on the sequence, as an INSERT does,
	# and nextval() itself, which asks it of the role, in PostgreSQL's words.
	# A segment's block not all given out is lost, as a session's is.
	q 0 "CREATE TABLE sqo (id bigserial, x int) DISTRIBUTED BY (x);
	     CREATE TABLE sqoi (id int GENERATED ALWAYS AS IDENTITY (CACHE 7), x int) DISTRIBUTED BY (x);
	     CREATE ROLE sqo_user LOGIN; GRANT SELECT ON o TO sqo_user; GRANT INSERT, SELECT ON sqo, sqoi TO sqo_user;" >/dev/null 2>&1
	n=$(q 0 "SELECT count(*) FROM o;")
	plan=$(q 0 "EXPLAIN (COSTS OFF) INSERT INTO sqo (x) SELECT a FROM o;")
	out=$(q 0 "INSERT INTO sqo (x) SELECT a FROM o;
		SELECT count(*) || ' ' || count(DISTINCT id) || ' ' || min(id) || ' ' || max(id) FROM sqo;
		SELECT nextval('sqo_id_seq');")
	[[ "$plan" == *"Insert on sqo"*"Optimizer: GPORCA"* ]] && [ "$out" = "$n $n 1 $n
$((n + 1))" ] && ok "a serial column's values, taken on the segments from the coordinator's sequence" \
		|| notok "a serial column under ORCA" "$out / $plan"
	n=$(q 0 "SELECT count(*) FROM o WHERE a <= 100;")
	out=$(q 0 "SET ROLE sqo_user; INSERT INTO sqoi (x) SELECT a FROM o WHERE a <= 100;
		SELECT count(*) || ' ' || count(DISTINCT id) || ' ' || min(id) || ' ' || (max(id) < count(*) + 2 * 7) FROM sqoi;")
	out2=$(q 0 "SET ROLE sqo_user; INSERT INTO sqo (x) SELECT a FROM o WHERE a < 5;")
	[ "$out" = "$n $n 1 true" ] && [[ "$out2" == *"permission denied for sequence sqo_id_seq"* ]] \
		&& ok "an identity column's, a block of its CACHE at a time, for a role with no privilege on it; nextval() of the role's, refused" \
		|| notok "an identity column under ORCA" "$out / $out2"
	n=$(q 0 "SELECT count(*) FROM o;")
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"BEGIN; CREATE SEQUENCE sqo_new; SELECT count(nextval('sqo_new')) FROM o; ROLLBACK;" \
		"CREATE TEMP SEQUENCE sqo_temp; SELECT count(nextval('sqo_temp')) FROM o;" \
		"BEGIN READ ONLY; SELECT count(nextval('sqo_id_seq')) FROM o; ROLLBACK;" | qf 0)
	case "$out" in
		*"a sequence's value taken in a slice the segments run"*"$n"*"a sequence's value taken in a slice the segments run"*"$n"*"cannot execute nextval() in a read-only transaction"*)
			ok "a sequence this transaction made, or a temporary one, is the planner's; a read-only transaction's, refused" ;;
		*) notok "sequences ORCA leaves to the planner" "$out" ;;
	esac

	# A BRIN index is ORCA's where Cloudberry's ORCA takes it -- over values
	# in the order of the table's pages, as Cloudberry's brin test has them --
	# its statistics from the segments after VACUUM ANALYZE.
	q 0 "CREATE TABLE wbr (a int, b int) DISTRIBUTED BY (a);
	     INSERT INTO wbr SELECT x / 100, x % 100 FROM generate_series(1, 200000) x;
	     CREATE INDEX wbr_a ON wbr USING brin (a) WITH (pages_per_range = 2);" >/dev/null
	q 0 "VACUUM ANALYZE wbr;" >/dev/null
	orca_same "a BRIN index over values in the order of the pages: a bitmap scan of it" \
		"SELECT count(*), sum(b) FROM wbr WHERE a = 1;" "Bitmap Index Scan on wbr_a"

	# A key of two columns: ORCA's core gives it no direct dispatch, and the
	# Query's own conditions do, as the planner's; a row of constants is
	# written on its segment alone.
	q 0 "CREATE TABLE mo (a int, b int) DISTRIBUTED BY (a, b); INSERT INTO mo SELECT i, i % 3 FROM generate_series(1, 300) i; ANALYZE mo;" >/dev/null
	orca_same "a key of two columns fixed: direct dispatch, from the query's conditions" \
		"SELECT count(*) FROM mo WHERE a = 7 AND b = 1;" "Gather Motion 1:1  (slice1; segments: 1)"
	out=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" "INSERT INTO mo VALUES (1000, 1);" \
		"SELECT count(*) FROM mo WHERE a = 1000 AND b = 1;" | qf 0 | grep -o 'INFO:  (slice.*\|^[0-9]*$' | tr '\n' '/')
	[ "$out" = "INFO:  (slice 0) Dispatch command to SINGLE content/INFO:  (slice 1) Dispatch command to SINGLE content/1/" ] \
		&& ok "... and ORCA's INSERT of a row of constants into it is sent to that row's segment, where it is then found" \
		|| notok "ORCA's direct dispatch of a two-column key" "$out"
	orca_same "a join of tables distributed alike, on the segments" \
		"SELECT count(*) FROM o o1 JOIN o o2 USING (a) WHERE o2.b = 1;"
	orca_same "a replicated table, read from one segment" \
		"SELECT count(*), max(name) FROM ro;" "Gather Motion 1:1"

	# A record of no declared type travels with its row type described
	# (gp_record.c), where its typmod alone is the number the sending process
	# gave the row type: made on the segments and gathered; sorted there and
	# merged; one a query of gp_dist_random() alone gives on every segment;
	# and a PL/pgSQL record's field, which ORCA's folding makes a constant
	# the segments evaluate.
	q 0 "CREATE FUNCTION rec_of(int, OUT int, OUT text) LANGUAGE sql
	     AS 'SELECT \$1 - 1, \$1::text || ''z''';" >/dev/null
	orca_same "a record of no declared type, made on the segments and gathered" \
		"SELECT a, rec_of(a) FROM o WHERE a < 6 ORDER BY a;" "Gather Motion"
	orca_same "... grouped by, sorted on the segments and merged" \
		"SELECT r, count(*) FROM (SELECT rec_of(a % 5) AS r FROM o) s GROUP BY r ORDER BY r;" \
		"Merge Key"
	# ... and a set of them a ProjectSet returns on the segments, which a
	# Result above it relabels: in the ProjectSet the set-returning call has
	# to stay at the top of its column (a segment's Assert).
	orca_same "... and a set of them a set-returning function returns for each row, on the segments" \
		"SELECT count(*), count(DISTINCT kw) FROM (SELECT pg_get_keywords() AS kw FROM o WHERE a < 4) s;" \
		"ProjectSet"
	out=$(q 0 "SELECT rec_of(gp_execution_segment()) FROM gp_dist_random('gp_id') ORDER BY 1;" | tr '\n' '/')
	[ "$out" = "(-1,0z)/(0,1z)/" ] \
		&& ok "... one each segment makes in a query of gp_dist_random('gp_id') alone" \
		|| notok "a record of no declared type from gp_dist_random('gp_id')" "$out"
	q 0 "CREATE FUNCTION rec_param() RETURNS bigint LANGUAGE plpgsql AS \$\$
	     DECLARE r record; n record; c bigint;
	     BEGIN
	       SELECT 1 AS i, 2 AS j INTO r;
	       SELECT r AS rec, 'x' AS f INTO n;
	       SELECT count(*) INTO c FROM o WHERE a > length(n.rec::text) + 990;
	       RETURN c;
	     END \$\$;" >/dev/null
	out=$(q 0 "SELECT rec_param();")
	[ "$out" = "5" ] \
		&& ok "... and a PL/pgSQL record's field, in a condition the segments evaluate" \
		|| notok "a PL/pgSQL record's field on the segments" "$out"
	orca_same "a correlated subquery, run on the segments" \
		"SELECT a, (SELECT name FROM ro WHERE ro.b = o.b) FROM o WHERE a < 4 ORDER BY a;" \
		"SubPlan"
	orca_same "two gathers, one slice each" \
		"SELECT a FROM o WHERE a IN (SELECT b FROM o WHERE a < 20) ORDER BY a;" \
		"(slice2; segments: 2)"
	# NOT IN, ORCA's anti-join, which PostgreSQL 19's joins cannot run: the
	# planner's hashed SubPlan, on the segments, over the inner rows ORCA
	# broadcasts to each; a NULL among them leaves no row, and a NULL outer
	# value is no row.
	orca_same "NOT IN: a hashed SubPlan on the segments, over the rows broadcast to each" \
		"SELECT count(*), sum(a) FROM o WHERE a NOT IN (SELECT x * 2 FROM po WHERE y < 3);" \
		"hashed SubPlan"
	orca_same "... none, a NULL among them" \
		"SELECT count(*) FROM o WHERE a NOT IN (SELECT CASE WHEN x = 7 THEN NULL ELSE x END FROM po);"
	orca_same "... and an outer NULL no row" \
		"SELECT count(*), count(a) FROM (SELECT NULLIF(a, 5) AS a FROM o) s WHERE a NOT IN (SELECT x FROM po WHERE y = 0);"

	# A CTE in a slice the segments run: Cloudberry's cross-slice
	# ShareInputScan, whose producer writes the CTE's rows to files each
	# segment keeps, which the consumers in its slice and in others read
	# once they are all there (compat/sharedscan.c).  In such a plan the
	# coordinator's aggregate of gathered rows, which ORCA sends back to the
	# segments, runs on one of them, and every slice streams.
	q 0 "CREATE TABLE sh (a int, b int) DISTRIBUTED BY (a);
	     INSERT INTO sh SELECT i % 100, i FROM generate_series(1, 10000) i; ANALYZE sh;" >/dev/null
	orca_same "a CTE read twice in the slice that produces it: a Shared Scan" \
		"WITH c AS (SELECT a, count(*) cnt FROM sh GROUP BY a) SELECT count(*), sum(c1.cnt) FROM c c1 JOIN c c2 USING (a) WHERE c1.cnt > 5;" \
		"Shared Scan (share slice:id"
	orca_same "... and in another slice, below a Redistribute Motion" \
		"WITH c AS (SELECT a, b FROM sh WHERE b % 3 = 0) SELECT count(*), sum(c1.b) FROM c c1 JOIN c c2 ON c1.b = c2.a;" \
		"Redistribute Motion 2:2"
	orca_same "grouping sets, which ORCA aggregates over a shared CTE, a slice each" \
		"SELECT a % 3, b % 4, count(*) FROM sh GROUP BY CUBE (a % 3, b % 4) ORDER BY 1, 2;" \
		"Sequence"
	orca_same "the aggregate of gathered rows ORCA sends back to the segments, on one of them" \
		"WITH r AS (SELECT b % 37 AS s, sum(a) AS t FROM sh GROUP BY 1) SELECT s, t FROM r WHERE t = (SELECT max(t) FROM r) ORDER BY 1;" \
		"Motion 1:2"
	# ... in a subquery's plan too, which a translator of its own translates:
	# the aggregate a HAVING compares with, as TPC-DS's query 24 has it.
	sql="WITH s AS (SELECT a, b % 7 AS k, sum(b) AS paid FROM sh GROUP BY a, b % 7) SELECT a, sum(paid) FROM s WHERE k = 3 GROUP BY a HAVING sum(paid) > (SELECT 0.05 * avg(paid) FROM s) ORDER BY a;"
	plan=$(q 0 "SET gp.optimizer_enforce_subplans = on; EXPLAIN (COSTS OFF) $sql" | tr '\n' '|')
	got=$(q 0 "SET gp.optimizer_enforce_subplans = on; $sql")
	want=$(q 0 "SET gp.optimizer = off; $sql")
	case "$plan" in
		*"SubPlan"*"Motion 1:2"*"Shared Scan"*"Optimizer: GPORCA"*)
			[ -n "$got" ] && [ "$got" = "$want" ] \
				&& ok "... and in a subquery's plan, which is translated apart" \
				|| notok "a shared CTE's aggregate in a SubPlan" "ORCA: $got / planner: $want" ;;
		*) notok "a shared CTE's aggregate in a SubPlan: the plan" "$plan" ;;
	esac
	out=$(q 0 "SET gp.optimizer_enable_hashjoin = off; SET gp.optimizer_enable_mergejoin = off;
		EXPLAIN (COSTS OFF) WITH c AS (SELECT a, b FROM sh WHERE b < 400) SELECT count(*) FROM c c1 JOIN c c2 ON c1.a = c2.a AND c1.b < c2.b;" | tr '\n' '|')
	got=$(q 0 "SET gp.optimizer_enable_hashjoin = off; SET gp.optimizer_enable_mergejoin = off;
		WITH c AS (SELECT a, b FROM sh WHERE b < 400) SELECT count(*) FROM c c1 JOIN c c2 ON c1.a = c2.a AND c1.b < c2.b;")
	want=$(q 0 "SET gp.optimizer = off; WITH c AS (SELECT a, b FROM sh WHERE b < 400) SELECT count(*) FROM c c1 JOIN c c2 ON c1.a = c2.a AND c1.b < c2.b;")
	case "$out" in
		*"Nested Loop"*"Shared Scan"*"Optimizer: GPORCA"*)
			[ "$got" = "$want" ] && ok "... a consumer read again for each row of a nested loop" \
				|| notok "a Shared Scan read again" "ORCA: $got / planner: $want" ;;
		*) notok "a Shared Scan on a nested loop's inner side: the plan" "$out" ;;
	esac
	got=$(q 0 "SET statement_timeout = '60s'; WITH c AS (SELECT a, b FROM sh) SELECT c1.a FROM c c1 JOIN c c2 ON c1.b = c2.a LIMIT 3;" | grep -c '^[0-9][0-9]*$')
	after=$(q 0 "SELECT count(*) FROM sh;")
	[ "$got" = 3 ] && [ "$after" = 10000 ] \
		&& ok "... a LIMIT that stops the Gather before the consumers have read, and nothing waits" \
		|| notok "a Shared Scan below a LIMIT" "$got rows, then $after"
	out=$(q 0 "SET statement_timeout = '60s'; WITH c AS (SELECT a, b FROM sh) SELECT count(*) FROM c c1 JOIN c c2 ON c1.b = c2.a WHERE 1 / (c2.b - 50) > -1;")
	case "$out" in
		*"division by zero"*) ok "... an error in a consumer's slice ends the statement, nothing waiting" ;;
		*) notok "an error with a Shared Scan" "$out" ;;
	esac
	# EXPLAIN prints a Sequence's producer before the plan that reads it, in
	# the order they run, as Cloudberry's does; and EXPLAIN ANALYZE of it
	# runs the producers on the segments alone, not in the coordinator's
	# description of the fragment.
	out=$(q 0 "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) WITH c AS (SELECT a, b FROM sh WHERE b % 3 = 0) SELECT count(*), sum(c1.b) FROM c c1 JOIN c c2 ON c1.b = c2.a;" | tr '\n' '|')
	case "$out" in
		*"Sequence"*"|"*"->  Shared Scan (share slice:id "*"|"*"->  Result"*"|"*"Seq Scan on sh"*"Optimizer: GPORCA"*)
			ok "... EXPLAIN shows its producer first, and EXPLAIN ANALYZE runs" ;;
		*) notok "EXPLAIN of a shared CTE" "$out" ;;
	esac
	left=$(ls -d "$(datadir 1)"/base/pgsql_tmp/*.fileset "$(datadir 2)"/base/pgsql_tmp/*.fileset 2>/dev/null | wc -l)
	[ "$left" = 0 ] && ok "... and the segments keep none of the shared CTEs' files after" \
		|| notok "the shared CTEs' files, left on the segments" "$(ls -d "$(datadir 1)"/base/pgsql_tmp/* "$(datadir 2)"/base/pgsql_tmp/* 2>/dev/null | tr '\n' ' ')"
	# gp_core relays a slice at a time with gp.interconnect_type = relay, and
	# a slice that reads a temporary table but the one its Gather sends,
	# which the writer runs: a plan whose shared CTE is in a slice it relays
	# is the planner's; one that reads a temporary table only where the
	# writer runs is ORCA's.
	shared_relayed() {			# shared_relayed <what> <setup> <table> <ORCA's or not>
		local out
		out=$(printf '%s\n' "$2" "SET gp.optimizer_trace_fallback = on;" \
			"WITH c AS (SELECT a, b FROM $3 WHERE b % 3 = 0) SELECT count(*), sum(c1.b) FROM c c1 JOIN c c2 ON c1.b = c2.a;" | qf 0 | tr '\n' '|')
		case "$4:$out" in
			relayed:*"a CTE read in more than one slice, whose slices cannot all run at once"*"1122|57222|"*)
				ok "$1" ;;
			orca:*"GPORCA failed"*) notok "$1" "$out" ;;
			orca:*"1122|57222|"*) ok "$1" ;;
			*) notok "$1" "$out" ;;
		esac
	}
	shared_relayed "... left to the planner with gp.interconnect_type = relay" \
		"SET gp.interconnect_type = relay;" sh relayed
	shared_relayed "... and ORCA's where it reads a temporary table in the writer's own slice" \
		"CREATE TEMP TABLE sht AS SELECT * FROM sh DISTRIBUTED BY (a); ANALYZE sht;" sht orca

	# A CTE the coordinator's slice produces -- an aggregate of gathered
	# rows, a LIMIT of them -- is PostgreSQL's, in the coordinator's process,
	# and ORCA's slices that send the coordinator's rows to the segments read
	# it there: the Gather above relays them in that process (gp_motion.c),
	# and keeps each to its end, the first CteScan holding what the others
	# read.
	orca_same "a CTE the coordinator produces, read by two of its slices that send to the segments" \
		"WITH m(mx) AS (SELECT max(b) FROM sh) SELECT count(*), sum(sh.b) FROM sh, m WHERE sh.b < (SELECT mx FROM m) AND sh.b > m.mx - 500;" \
		"CTE Scan on cte0 cte0_1"
	orca_same "... and a LIMIT's, sent one way and the other" \
		"WITH l AS (SELECT a, b FROM sh ORDER BY b LIMIT 20) SELECT count(*), sum(l1.b) FROM l l1 JOIN sh USING (a) JOIN l l2 ON l2.b = sh.b;" \
		"CTE Scan on cte0 cte0_1"

	# MERGE, which ORCA has no operator for, rides a SELECT: ORCA plans the
	# join, on the segments, and the explicit write writes each action's rows
	# where they are, as the planner's route does (gp_orca's merge.c) --
	# gp_core's Row Identity node making each target row's segment and ctid
	# the ctid the explicit write knows it by, the target's row come up whole.
	# Its source may be a VALUES list here, the explicit write re-checking no
	# row through EvalPlanQual; a row whose key changes is moved.
	q 0 "CREATE TABLE omt (id int, v text, n int) DISTRIBUTED BY (id);
	     INSERT INTO omt SELECT g, 'o' || g, g FROM generate_series(1, 200) g;
	     CREATE TABLE oms (id int, v text, d boolean) DISTRIBUTED BY (id);
	     INSERT INTO oms SELECT g * 3, 's' || g, g % 5 = 0 FROM generate_series(1, 100) g;
	     ANALYZE omt; ANALYZE oms;" >/dev/null
	merge_same() {				# merge_same <what> <merge> [what EXPLAIN must say] [check]
		local plan want got
		local check="${4:-SELECT count(*), sum(id), sum(n), string_agg(DISTINCT v, ',' ORDER BY v) FROM omt;}"
		plan=$(q 0 "EXPLAIN (COSTS OFF) $2")
		want=$(printf '%s\n' "SET gp.optimizer = off;" "BEGIN;" "$2;" "$check" "ROLLBACK;" | qf 0)
		got=$(printf '%s\n' "BEGIN;" "$2;" "$check" "ROLLBACK;" | qf 0)
		case "$plan" in
			*"${3:-Row Identity}"*"Optimizer: GPORCA"*) ;;
			*) notok "$1: planned by ORCA" "$plan"; return ;;
		esac
		[ "$got" = "$want" ] && [ -n "$got" ] && ok "$1" \
			|| notok "$1: the same rows as the planner's" "ORCA: $got / planner: $want"
	}
	merge_same "a MERGE under ORCA: its join on the segments, the explicit write over it" \
		"MERGE INTO omt t USING oms s ON t.id = s.id WHEN MATCHED AND s.d THEN DELETE WHEN MATCHED THEN UPDATE SET v = s.v, n = t.n + 1000 WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.v)"
	merge_same "... from a VALUES list" \
		"MERGE INTO omt t USING (VALUES (5, 'five'), (500, 'new')) s(id, v) ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED THEN INSERT (id, v) VALUES (s.id, s.v)"
	merge_same "... and one that changes the key, a Split" \
		"MERGE INTO omt t USING oms s ON t.id = s.id WHEN MATCHED AND s.id < 100 THEN UPDATE SET id = t.id + 1000"
	# WHEN NOT MATCHED BY SOURCE: what tells a target row the source has no
	# row for is what is null exactly there -- the source table's ctid, a
	# VALUES list's or a subquery's column of its own -- where the planner
	# tests the source's whole row, which ORCA does not take.
	merge_same "... WHEN NOT MATCHED BY SOURCE, a table's" \
		"MERGE INTO omt t USING oms s ON t.id = s.id WHEN MATCHED AND s.d THEN DELETE WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.v) WHEN NOT MATCHED BY SOURCE AND t.id % 7 = 0 THEN DELETE WHEN NOT MATCHED BY SOURCE THEN UPDATE SET n = -t.n"
	# A partitioned target: each row's partition, its tableoid, carried up
	# from the scan that read it, a result relation for each partition, and
	# the target's row as its partition has it -- one of another column
	# order, one dropped -- a row moved between partitions and segments.
	q 0 "CREATE TABLE opm (id int, v text, n int) DISTRIBUTED BY (id) PARTITION BY RANGE (id);
	     CREATE TABLE opm1 PARTITION OF opm FOR VALUES FROM (0) TO (100);
	     CREATE TABLE opm2 (n int, junk int, v text, id int) DISTRIBUTED BY (id);
	     ALTER TABLE opm2 DROP COLUMN junk;
	     ALTER TABLE opm ATTACH PARTITION opm2 FOR VALUES FROM (100) TO (1000);
	     INSERT INTO opm SELECT g, 'o' || g, g FROM generate_series(1, 300, 2) g;" >/dev/null
	merge_same "... into a partitioned table, a result relation for each partition" \
		"MERGE INTO opm t USING oms s ON t.id = s.id WHEN MATCHED AND s.d THEN DELETE WHEN MATCHED AND t.id = 9 THEN UPDATE SET id = 105, v = 'moved' WHEN MATCHED THEN UPDATE SET v = s.v, n = t.n + 1000 WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.v) WHEN NOT MATCHED BY SOURCE AND t.id > 250 THEN DELETE" \
		"Dynamic Seq Scan" \
		"SELECT tableoid::regclass, count(*), sum(id), sum(n), string_agg(v, ',' ORDER BY id) FROM opm GROUP BY 1 ORDER BY 1;"
	merge_same "... a VALUES list's and a subquery's" \
		"MERGE INTO omt t USING (VALUES (5, 'five'), (500, 'new')) s(id, v) ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED BY SOURCE AND t.id > 150 THEN DELETE;
		 MERGE INTO omt t USING (SELECT id, max(v) AS v FROM oms WHERE id < 200 GROUP BY id) s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED BY SOURCE THEN UPDATE SET n = 0"
	q 0 "MERGE INTO omt t USING oms s ON t.id = s.id WHEN MATCHED AND s.id < 60 THEN UPDATE SET id = t.id + 1000 WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.v);" >/dev/null
	w1=$(q 1 "SELECT count(*) FROM omt WHERE expected_seg(id, 2) <> 0;"); w2=$(q 2 "SELECT count(*) FROM omt WHERE expected_seg(id, 2) <> 1;")
	out=$(q 0 "SELECT count(*) FROM omt WHERE id IN (1003, 1057, 201, 300);")
	[ "$w1|$w2|$out" = "0|0|4" ] && ok "... each row it wrote on the segment it hashes to" \
		|| notok "the rows ORCA's MERGE wrote, where they are" "misplaced $w1 $w2 / $out"

	# gp_segment_id is ORCA's system column, which its plan computes where
	# the row is read: a query naming it is ORCA's, a random table's
	# included, in a join too, where PostgreSQL's gather cannot give it; a
	# condition on it is direct dispatch; and a random table's UPDATE and
	# DELETE are ORCA's, which route by it.
	orca_same "gp_segment_id under ORCA: the segment that holds each row" \
		"SELECT gp_segment_id, count(*) FROM o GROUP BY 1 ORDER BY 1;"
	orca_same "a query of gp_dist_random('gp_id') and a function, on every segment" \
		"SELECT gp_segment_id, current_setting('port') FROM gp_dist_random('gp_id') ORDER BY 1;" \
		"Function Scan on segment_query"
	q 0 "CREATE TABLE orr (a int, b int) DISTRIBUTED RANDOMLY; INSERT INTO orr SELECT i, i FROM generate_series(1, 100) i; ANALYZE orr;" >/dev/null
	orca_same "a random table's gp_segment_id, and a condition on it asked of that segment alone" \
		"SELECT count(*) FROM orr WHERE gp_segment_id = 1;" "Gather Motion 1:1  (slice1; segments: 1)"
	plan=$(q 0 "EXPLAIN (COSTS OFF) SELECT count(*) FROM orr r JOIN orr s USING (a) WHERE r.gp_segment_id = s.gp_segment_id;")
	got=$(q 0 "SELECT count(*) FROM orr r JOIN orr s USING (a) WHERE r.gp_segment_id = s.gp_segment_id;")
	out=$(printf '%s\n' "EXPLAIN (COSTS OFF) UPDATE orr SET b = b + 1 WHERE a <= 10;" \
		"UPDATE orr SET b = b + 1 WHERE a <= 10;" "DELETE FROM orr WHERE a > 90;" \
		"SELECT count(*), sum(b) FROM orr;" | qf 0 | tr '\n' ' ')
	case "$plan|$got|$out" in
		*"Optimizer: GPORCA"*"|100|"*"Update on orr"*"Optimizer: GPORCA"*"90|4105 ")
			ok "a random table's gp_segment_id above a join, and its UPDATE and DELETE, are ORCA's" ;;
		*) notok "a random table's gp_segment_id and writes under ORCA" "$plan / $got / $out" ;;
	esac

	# The slice table, in PlannedStmt.extension_state, as Cloudberry's
	# EXPLAIN (SLICETABLE) would print it.
	out=$(q 0 "SELECT string_agg(concat_ws(',', slice, parent, gang, segments, direct_segment), ' ' ORDER BY slice) FROM gp_orca.slices('SELECT y, count(*) FROM o JOIN po ON o.b = po.y GROUP BY y');")
	out2=$(q 0 "SELECT string_agg(concat_ws(',', slice, gang, direct_segment), ' ' ORDER BY slice) FROM gp_orca.slices('SELECT * FROM o WHERE a = 42');")
	[ "$out" = "0,unallocated,1 1,0,primary reader,2 2,1,primary reader,2 3,1,primary reader,2" ] && \
		[[ "$out2" == "0,unallocated 1,primary reader,"[01] ]] \
		&& ok "the slice table: each slice, the one it sends to, its gang, and direct dispatch's segment" \
		|| notok "the slice table" "$out / $out2"

	# EXPLAIN ANALYZE: the segments' part as the segment that returned the
	# most rows ran it, as Cloudberry's winner, where the coordinator only
	# describes it (gp_explain.c).
	most=$(q 0 "SELECT max(n) FROM (SELECT gp_segment_id, count(*) n FROM o GROUP BY 1) s;")
	out=$(q 0 "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM o;")
	case "$out" in
		*"Gather Motion 2:1"*"(actual rows=2"*"Seq Scan on o (actual rows=$most.00 loops=1)"*)
			ok "EXPLAIN ANALYZE: the segments' part as the segment with the most rows ran it" ;;
		*) notok "EXPLAIN ANALYZE of a Motion" "$most / $out" ;;
	esac

	# What the segments' part wrote to the WAL, its slices' memory, each
	# segment's run of a node with gp.enable_explain_allstat -- after a
	# LIMIT above the Motion too, whose segments' part is ended before the
	# plan is printed, a segment the coordinator's LIMIT stopped short having
	# sent fewer than its own LIMIT's rows -- and a sort that spilled,
	# Cloudberry's words for them all.
	q 0 "CREATE TABLE ow (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	out=$(q 0 "EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF) INSERT INTO ow SELECT a, b FROM o;
			   EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF) UPDATE ow SET b = b + 1;
			   EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF) DELETE FROM ow;" | tr '\n' '|')
	case "$out" in
		*"Insert on ow (actual rows=0.00 loops=1)|        WAL: records="*"Update on ow (actual rows=0.00 loops=1)|        WAL: records="*"Delete on ow (actual rows=0.00 loops=1)|        WAL: records="*)
			ok "EXPLAIN (ANALYZE, WAL) of ORCA's INSERT, UPDATE and DELETE: what the segments wrote" ;;
		*) notok "EXPLAIN (ANALYZE, WAL) of ORCA's writes" "$out" ;;
	esac
	out=$(q 0 "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT * FROM o;" | tr '\n' '|')
	case "$out" in
		*"(slice0)    Executor memory: "*" bytes.|  (slice1)    Executor memory: "*" bytes avg x 2 workers, "*" bytes max (seg"*)
			ok "EXPLAIN ANALYZE: each slice's memory, the coordinator's and the segments'" ;;
		*) notok "EXPLAIN ANALYZE's slice statistics under ORCA" "$out" ;;
	esac
	out=$(q 0 "SET gp.enable_explain_allstat = on;
			   EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT * FROM o LIMIT 3;")
	case "$out" in
		*"Seq Scan on o (actual rows=3.00 loops=1)"*"allstat: seg_firststart_total_ntuples/seg0_"*"_"[123]"/seg1_"*"_"[123]"//end"*)
			ok "gp.enable_explain_allstat: each segment's run, a LIMIT's left open included" ;;
		*) notok "gp.enable_explain_allstat under ORCA" "$out" ;;
	esac
	out=$(q 0 "EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT k, repeat(s, 8) r FROM bo ORDER BY r, k;")
	case "$out" in
		*"Sort (actual rows="*"work_mem: "*"kB  Segments: 2  Max: "*"kB (segment "*")  Workfile: (2 spilling)"*)
			ok "a sort that spilled on both segments: Cloudberry's work_mem line" ;;
		*) notok "EXPLAIN ANALYZE's work_mem line" "$out" ;;
	esac

	# The Motions between segments.
	orca_same "a GROUP BY off the key: partial aggregates, redistributed by it" \
		"SELECT b, count(*), avg(a) FROM o GROUP BY b ORDER BY b;" \
		"Redistribute Motion 2:2  (slice2; segments: 2)"
	# A reader is a member of its writer's lock group, and cannot lead a group
	# of its own, which starting parallel workers makes a backend: what a
	# function the reader runs plans, it plans without them, whatever the
	# function sets (gp_share.c).  Before, the segments' readers failed an
	# assertion in BecomeLockGroupLeader().
	q 0 "CREATE FUNCTION wants_workers(x int) RETURNS int LANGUAGE plpgsql IMMUTABLE
	     SET max_parallel_workers_per_gather = 2 SET debug_parallel_query = on
	     SET parallel_setup_cost = 0 SET parallel_tuple_cost = 0
	     AS \$\$ BEGIN PERFORM count(*) FROM pg_class WHERE oid > x; RETURN x; END \$\$;" >/dev/null
	orca_same "a function that asks for parallel workers, in the readers of a join's slices" \
		"SELECT count(*) FROM o JOIN po ON o.b = po.y WHERE wants_workers(o.a) > 0 AND wants_workers(po.x) > 0;" \
		"Redistribute Motion 2:2  (slice3; segments: 2)"

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

	# A Gather in the coordinator's own slice -- Cloudberry's entry DB --
	# below a Motion the coordinator sends from: the coordinator runs it as
	# it runs one above every fragment, and sends what it makes of the rows.
	orca_same "EXISTS as a LIMIT over a Gather, which the coordinator broadcasts back" \
		"SELECT count(*), sum(a) FROM o WHERE EXISTS (SELECT 1 FROM po WHERE po.x = 3);" \
		"Broadcast Motion 1:2"
	orca_same "and one whose Gather finds nothing" \
		"SELECT count(*) FROM o WHERE EXISTS (SELECT 1 FROM po WHERE po.x < 0);" \
		"Broadcast Motion 1:2"
	orca_same "an aggregate finished on the coordinator, broadcast back to a join" \
		"SELECT count(*), sum(o.a) FROM o, (SELECT max(y) AS m FROM po) s WHERE o.b < s.m;" \
		"Broadcast Motion 1:2"

	# A table hashed with Cloudberry's legacy cdbhash (a cdbhash_*_ops key),
	# joined off another's key: that one's rows are redistributed by the
	# legacy hash, or they would miss the rows they join.
	q 0 "CREATE TABLE lo (a int) DISTRIBUTED BY (a cdbhash_int4_ops);
	     INSERT INTO lo SELECT generate_series(1, 6);
	     CREATE TABLE lo2 (k int, a int) DISTRIBUTED BY (k cdbhash_int4_ops);
	     INSERT INTO lo2 SELECT i, i % 6 + 1 FROM generate_series(1, 60) i;
	     ANALYZE lo; ANALYZE lo2;" >/dev/null
	orca_same "a legacy-hashed table's join: the other side redistributed by the legacy hash" \
		"SELECT count(*), sum(lo2.k) FROM lo JOIN lo2 USING (a);" "Redistribute Motion"

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
	relay_same "streamed and relayed, the same rows: redistributed by the legacy hash" \
		"SELECT count(*), sum(lo2.k) FROM lo JOIN lo2 USING (a);"

	# udpifc: the same rows in UDP packets, each receiver acknowledging them
	# and saying how much room it has (gp_ic.c) -- through a window of one
	# small packet, and with packets and acknowledgements lost, as
	# Cloudberry's gp_udpic_drop* settings lose them: sent again, and the
	# sender whose receiver's room stays shut asks it what it has.
	udp_same() {				# udp_same <what> <settings> <sql>: tcp and udpifc agree
		local tcp udp
		tcp=$(q 0 "$3")
		udp=$(q 0 "SET gp.interconnect_type = udpifc; $2 $3")
		[ "$tcp" = "$udp" ] && [ -n "$tcp" ] && ok "$1" \
			|| notok "$1: tcp and udpifc rows agree" "tcp: $tcp / udpifc: $udp"
	}
	udp_same "udpifc, the same rows: two Motions below a Gather" "" \
		"SELECT y, count(*), sum(a) FROM o JOIN po ON o.b = po.y GROUP BY y ORDER BY y;"
	udp_same "udpifc, the same rows: sixty thousand rows between segments, a window of one small packet" \
		"SET gp.interconnect_queue_depth = 1; SET gp.max_packet_size = 600;" \
		"SELECT count(*), sum(length(s)) FROM (SELECT s, count(*) FROM bo GROUP BY s) x;"
	udp_same "udpifc, the same rows: a tenth of the packets lost, and a third of the acknowledgements" \
		"SET gp.udpic_dropxmit_percent = 10; SET gp.udpic_dropacks_percent = 30;" \
		"SELECT count(*), sum(length(s)) FROM (SELECT s, count(*) FROM bo GROUP BY s) x;"
	udp_same "udpifc, the same rows: a LIMIT whose receivers stop their senders" "" \
		"SELECT count(*) FROM (SELECT o.a FROM o JOIN po ON o.b = po.y LIMIT 7) x;"

	# notin's: a subquery's Broadcast inside another's.  Each slice reads its
	# Motion only until its ANY is decided, and its plan runs out before some
	# senders have sent it a packet -- which it waits for, to stop them, as it
	# would not hear them once idle; the inner Motion, which the fragment on
	# every segment carries, it leaves to the readers that receive it.
	q 0 "CREATE TABLE ni1 (c int) DISTRIBUTED BY (c);
		CREATE TABLE ni2 (c int) DISTRIBUTED BY (c);
		CREATE TABLE ni3 (c int) DISTRIBUTED BY (c);
		INSERT INTO ni1 SELECT generate_series(1, 10);
		INSERT INTO ni2 SELECT generate_series(1, 5);
		INSERT INTO ni3 VALUES (1), (2), (3);
		ANALYZE ni1; ANALYZE ni2; ANALYZE ni3;" >/dev/null
	udp_same "udpifc, the same rows: a subquery's Motion inside another's, each read until its ANY is decided" "" \
		"SELECT c FROM ni1 WHERE NOT c = ALL (SELECT c FROM ni2 WHERE NOT c > ALL (SELECT c FROM ni3)) ORDER BY c;"

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

	# A temporary table is read only by its session's own backend, the
	# writer: the slice that scans one is relayed through it first, and the
	# others stream, on readers -- this session's, which are its writer's
	# lock group on each segment.
	readers="SELECT sum(result::int) FROM gp.exec_on_segments('SELECT count(*) FROM pg_stat_activity WHERE leader_pid = pg_backend_pid()');"
	out=$(printf '%s\n' "CREATE TEMP TABLE tmpo (x int, y int) DISTRIBUTED BY (x);" \
		"INSERT INTO tmpo SELECT i, i % 7 FROM generate_series(1, 70) i;" "ANALYZE tmpo;" \
		"SELECT count(*) FROM o JOIN tmpo ON o.b = tmpo.y;" "$readers" | qf 0)
	[ "$out" = "7000
2" ] && ok "a temporary table: the slice that scans it relayed through the writer, the other streamed" \
		|| notok "a temporary table and streaming" "$out"

	# One scanned in a subplan's own part -- a replicated one, which ORCA
	# reads where a correlated subquery is called -- relays only the slice
	# that calls the subplan, which the plan says (GP_SUBPLAN_SLICES), and
	# not every slice of the statement.
	mk="CREATE TEMP TABLE tmpr (x int, y int) DISTRIBUTED REPLICATED;
		INSERT INTO tmpr SELECT i, i % 7 FROM generate_series(1, 70) i; ANALYZE tmpr;"
	sql="SELECT count(*), sum(o.a) FROM o JOIN po ON o.b = po.y WHERE o.a > (SELECT count(*) * 90 FROM tmpr WHERE tmpr.y = o.a % 7);"
	plan=$(printf '%s\n' "$mk" "EXPLAIN (COSTS OFF) $sql" | qf 0)
	out=$(printf '%s\n' "$mk" "$sql" "$readers" | qf 0)
	want=$(printf '%s\n' "$mk" "SET gp.optimizer = off;" "$sql" | qf 0)
	case "$plan" in
		*"SubPlan"*"Seq Scan on tmpr"*"Optimizer: GPORCA"*)
			[ -n "$want" ] && [ "$out" = "$want
2" ] && ok "a temporary table in a subplan's own part: the slice that calls it relayed, the other streamed" \
				|| notok "a temporary table in a subplan and streaming" "$out / planner: $want" ;;
		*) notok "a temporary table in a subplan: the plan" "$plan" ;;
	esac

	# The coordinator's own slice -- a function's rows, which it reads
	# itself, redistributed -- feeding a slice that a reader runs: the
	# writer keeps the rows it is sent in files the reader opens
	# (gp_motion_put_shared()).
	sql="SELECT count(*), sum(p2.x) FROM (SELECT o.b FROM o WHERE o.a IN (SELECT (random() * 0)::int + g FROM generate_series(1, 300) g)) s JOIN po p2 ON s.b = p2.y;"
	plan=$(q 0 "EXPLAIN (COSTS OFF) $sql")
	out=$(printf '%s\n' "$sql" "$readers" | qf 0)
	want=$(q 0 "SET gp.interconnect_type = relay; $sql")
	case "$plan" in
		*"Redistribute Motion 2:2"*"Redistribute Motion 1:2"*"Function Scan on generate_series"*)
			[ "$out" = "$want
4" ] && ok "the coordinator's slice feeding a reader's: its rows read from the files the writer keeps for it" \
				|| notok "the coordinator's slice and a reader" "$out / $want" ;;
		*) notok "the coordinator's slice below a reader's: the plan" "$plan" ;;
	esac
	# EXPLAIN ANALYZE shows what it counted, where it ran: here.
	out=$(q 0 "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $sql" | grep "Function Scan")
	case "$out" in
		*"Function Scan on generate_series (actual rows=300"*) ok "... and EXPLAIN ANALYZE shows what that slice counted" ;;
		*) notok "EXPLAIN ANALYZE of the coordinator's relayed slice" "$out" ;;
	esac
	# One that works only on the rows it receives -- a LIMIT over a
	# Gather, broadcast back -- runs on the first segment instead, a reader
	# of its own, and streams: the Gather below it is into that segment.
	sql="SELECT count(*), sum(p2.x) FROM (SELECT o.b FROM o WHERE EXISTS (SELECT 1 FROM po WHERE po.x = 3)) s JOIN po p2 ON s.b = p2.y;"
	plan=$(q 0 "EXPLAIN (COSTS OFF) $sql")
	out=$(printf '%s\n' "$sql" "$readers" | qf 0)
	want=$(q 0 "SET gp.interconnect_type = relay; $sql")
	case "$plan" in
		*"Broadcast Motion 1:2  (slice"*"; segments: 1)"*"Limit"*"Gather Motion 2:1  (slice"*"Filter: (x = 3)"*)
			[ "$out" = "$want
7" ] && ok "... and one that works on the rows it receives runs on a segment, and streams" \
				|| notok "the coordinator's slice moved to a segment" "$out / $want" ;;
		*) notok "the coordinator's slice moved to a segment: the plan" "$plan" ;;
	esac
	# One that makes its rows itself -- a VALUES list, whose DEFAULT takes
	# the next value of a temporary table's sequence -- stays the
	# coordinator's in a plan that shares no CTE: its sequence is the
	# coordinator's session's, which a segment's slice could not take.
	out=$(printf '%s\n' "CREATE TEMP TABLE tv (f1 serial, f2 text, f3 int DEFAULT 42) DISTRIBUTED BY (f1);" \
		"EXPLAIN (COSTS OFF) INSERT INTO tv (f2, f3) VALUES ('a', DEFAULT), ('b', 11), (upper('c'), 7 + 9);" \
		"INSERT INTO tv (f2, f3) VALUES ('a', DEFAULT), ('b', 11), (upper('c'), 7 + 9);" \
		"SELECT string_agg(concat_ws(':', f1, f2, f3), ' ' ORDER BY f1) FROM tv;" | qf 0)
	case "$out" in
		*gp_internal.nextval*) notok "a VALUES list's slice of the coordinator's own" "$out" ;;
		*"Redistribute Motion 1:2"*"nextval('tv_f1_seq'::regclass)"*"Values Scan"*"Optimizer: GPORCA"*"1:a:42 2:b:11 3:C:16")
			ok "... and one that makes its rows itself, a VALUES list taking a temporary sequence's values, stays the coordinator's" ;;
		*) notok "a VALUES list's slice of the coordinator's own" "$out" ;;
	esac

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

	# RETURNING, which ORCA never sees: the ModifyTable on the segments
	# evaluates the Query's list, and they send back the rows it gives, as
	# the Gather Motion over Cloudberry's ModifyTable does.  Each statement
	# runs in a transaction rolled back, under ORCA and under the planner,
	# and their rows are compared.
	orca_returning() {			# orca_returning <what> <sql> <what EXPLAIN must say>
		local plan orca pg
		plan=$(q 0 "EXPLAIN (COSTS OFF) $2")
		case "$plan" in
			*"$3"*"Optimizer: GPORCA"*) ;;
			*) notok "$1: the plan" "$plan"; return ;;
		esac
		orca=$(printf '%s\n' "BEGIN;" "$2" "ROLLBACK;" | qf 0 | sort)
		pg=$(printf '%s\n' "SET gp.optimizer = off;" "BEGIN;" "$2" "ROLLBACK;" | qf 0 | sort)
		[ -n "$orca" ] && [ "$orca" = "$pg" ] && ok "$1" \
			|| notok "$1: the same rows as the planner's" "ORCA: $orca / planner: $pg"
	}
	orca_returning "UPDATE ... RETURNING, old and new: the rows the segments wrote, gathered" \
		"UPDATE wo SET c = 'r' WHERE b < 20 RETURNING a, b, old.c, new.c;" \
		"Gather Motion 2:1  (slice1; segments: 2)"
	orca_returning "DELETE ... RETURNING the rows it deleted" \
		"DELETE FROM wo WHERE b > 480 RETURNING *;" "Delete on wo"
	orca_returning "INSERT ... SELECT ... RETURNING, redistributed and gathered" \
		"INSERT INTO wo SELECT y + 100, x, 'i' FROM po WHERE x < 30 RETURNING a, b, c;" \
		"Redistribute Motion 2:2"
	orca_returning "a row of constants' RETURNING, from its segment alone" \
		"INSERT INTO wo VALUES (7, 7, 'seven') RETURNING *;" \
		"Gather Motion 1:1  (slice1; segments: 1)"
	orca_returning "a random table's UPDATE ... RETURNING, each row written where it is" \
		"UPDATE orr SET b = -b WHERE a % 10 = 0 RETURNING a, b;" "Update on orr"
	orca_returning "a replicated table's INSERT ... RETURNING" \
		"INSERT INTO ro SELECT i, 'new' || i FROM generate_series(100, 104) i RETURNING *;" \
		"Insert on ro"

	# Every segment writes a replicated table's rows, and returns them: once
	# here, and counted once.  An UPDATE with RETURNING counts what it wrote.
	out=$("$PSQL" -X -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres -c "BEGIN" \
		-c "INSERT INTO ro SELECT i, 'n' FROM generate_series(300, 302) i RETURNING b" \
		-c "UPDATE wo SET c = 'n' WHERE a = 3 AND b < 60 RETURNING b % 2" -c "ROLLBACK" 2>&1 | tr '\n' '/')
	[ "$out" = "BEGIN/300/301/302/INSERT 0 3/$(q 0 "SELECT b % 2 FROM wo WHERE a = 3 AND b < 60;" | tr '\n' '/')UPDATE $(q 0 "SELECT count(*) FROM wo WHERE a = 3 AND b < 60;")/ROLLBACK/" ] \
		&& ok "RETURNING's rows, a replicated table's once, and the statements' counts" \
		|| notok "RETURNING's counts" "$out"

	# What stays the planner's: a split update's RETURNING, whose rows
	# gp_core's Split writes as a DELETE and an INSERT, and a subquery in
	# the list, which the planner would make a SubPlan of.
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" "BEGIN;" \
		"UPDATE wo SET a = a + 1 WHERE b = 3 RETURNING a;" \
		"UPDATE wo SET c = 's' WHERE b = 4 RETURNING (SELECT count(*) FROM po);" "ROLLBACK;" | qf 0)
	case "$out" in
		*"RETURNING from an UPDATE that moves rows"*"a subquery in RETURNING"*)
			ok "a split update's RETURNING and a subquery in the list are the planner's, and say why" ;;
		*) notok "RETURNING that ORCA declines" "$out" ;;
	esac

	# ON CONFLICT: each proposed row goes to the segment its key hashes to,
	# and the ModifyTable there checks the arbiter index the planner's
	# infer_arbiter_indexes() found -- every unique index of a distributed
	# table holds its key, so the row it meets is on that segment too.
	q 0 "CREATE TABLE wu (a int PRIMARY KEY, b int, c text) DISTRIBUTED BY (a); INSERT INTO wu SELECT i, i, 'x' || i FROM generate_series(1, 100) i;" >/dev/null
	q 0 "CREATE TABLE wur (a int PRIMARY KEY, b int) DISTRIBUTED REPLICATED; INSERT INTO wur SELECT i, i FROM generate_series(1, 10) i;" >/dev/null
	orca_returning "ON CONFLICT DO UPDATE on the segments: EXCLUDED, the row met, and a condition" \
		"INSERT INTO wu SELECT i, i * 10, 'n' FROM generate_series(95, 110) i ON CONFLICT (a) DO UPDATE SET b = excluded.b + wu.b WHERE wu.b % 2 = 0 RETURNING *;" \
		"Conflict Resolution: UPDATE"
	orca_returning "ON CONFLICT DO NOTHING on the segments" \
		"INSERT INTO wu VALUES (1, 0, 'z'), (200, 0, 'z') ON CONFLICT (a) DO NOTHING RETURNING a, c;" \
		"Conflict Resolution: NOTHING"
	orca_returning "ON CONFLICT DO SELECT on the segments" \
		"INSERT INTO wu VALUES (7, 0, 'z'), (300, 0, 'z') ON CONFLICT (a) DO SELECT RETURNING a, b;" \
		"Conflict Resolution: SELECT"
	orca_returning "ON CONFLICT DO UPDATE of a replicated table, every segment's copy alike" \
		"INSERT INTO wur VALUES (1, 100), (20, 20) ON CONFLICT (a) DO UPDATE SET b = excluded.b + wur.b RETURNING *;" \
		"Conflict Resolution: UPDATE"
	out=$(printf '%s\n' "BEGIN;" "INSERT INTO wur VALUES (2, 200), (30, 30) ON CONFLICT (a) DO UPDATE SET b = excluded.b;" \
		"SELECT count(DISTINCT (a, b)), count(*) FROM gp_dist_random('wur') WHERE a IN (2, 30);" "ROLLBACK;" | qf 0)
	[ "$out" = "2|4" ] && ok "... each of its segments with the same two rows" \
		|| notok "ON CONFLICT of a replicated table on every segment" "$out"

	# An UPDATE or DELETE that reads another table: without the deadlock
	# detector the statement holds its target in ExclusiveLock, so no row of
	# it changes under the statement and nothing is re-checked; ORCA joins
	# where the rows are, and each target row is written on its segment.
	# Each runs in a transaction rolled back, under ORCA and the planner, and
	# what a query reads afterwards is compared.
	orca_write() {				# orca_write <what> <sql> <check> <what EXPLAIN must say>
		local plan orca pg
		plan=$(q 0 "EXPLAIN (COSTS OFF) $2")
		case "$plan" in
			*"$4"*"Optimizer: GPORCA"*) ;;
			*) notok "$1: the plan" "$plan"; return ;;
		esac
		orca=$(printf '%s\n' "BEGIN;" "$2" "$3" "ROLLBACK;" | qf 0)
		pg=$(printf '%s\n' "SET gp.optimizer = off;" "BEGIN;" "$2" "$3" "ROLLBACK;" | qf 0)
		[ -n "$orca" ] && [ "$orca" = "$pg" ] && ok "$1" \
			|| notok "$1: the same rows as the planner's" "ORCA: $orca / planner: $pg"
	}
	orca_write "UPDATE ... FROM another table: joined on the segments, each row written where it is" \
		"UPDATE wu SET c = 'u' || po.y FROM po WHERE wu.b = po.x;" \
		"SELECT count(*), string_agg(DISTINCT c, ',' ORDER BY c) FROM wu WHERE c LIKE 'u%';" \
		"Redistribute Motion 2:2"
	orca_write "DELETE ... USING another table" \
		"DELETE FROM wu USING po WHERE wu.a = po.x * 3;" \
		"SELECT count(*), sum(a) FROM wu;" "Delete on wu"

	# A RETURNING that reads the other table: its columns, and its whole
	# row, carried up from its scan to the write, through the join and the
	# Motions, as the ctid is; the write reads them from its input row, as
	# the planner's does.
	orca_returning "UPDATE ... FROM another table, RETURNING its columns beside old and new" \
		"UPDATE wu SET c = 'f' || po.y FROM po WHERE wu.b = po.x RETURNING wu.a, po.x, po.y, old.c, new.c;" \
		"Update on wu"
	orca_returning "DELETE ... USING another table, RETURNING its row" \
		"DELETE FROM wu USING po WHERE wu.a = po.x AND po.y = 3 RETURNING wu.*, po.y, po;" \
		"Delete on wu"
	orca_write "DELETE ... WHERE IN a subquery, a semi-join" \
		"DELETE FROM wu WHERE a IN (SELECT x FROM po WHERE y = 3);" \
		"SELECT count(*), sum(a) FROM wu;" "Delete on wu"
	orca_write "UPDATE ... WHERE EXISTS" \
		"UPDATE wu SET b = -b WHERE EXISTS (SELECT 1 FROM po WHERE po.x = wu.a AND po.y > 4);" \
		"SELECT count(*), sum(b) FROM wu;" "Update on wu"
	orca_write "an update in place whose condition fixes the key: to that segment alone" \
		"UPDATE wu SET b = b + 1 WHERE a = 7;" \
		"SELECT count(*), sum(b) FROM wu;" "Dispatch  (slice1; segments: 1)"
	orca_write "an UPDATE of the key joined to another table: a Split, each row moved once" \
		"UPDATE wu SET a = a + 1000 FROM po WHERE wu.a = po.x AND po.y = 2;" \
		"SELECT count(*), sum(a), count(*) FILTER (WHERE a > 1000) FROM wu;" "Split Update"
	orca_write "a random table's UPDATE ... FROM, routed back by where each row is" \
		"UPDATE orr SET b = orr.b + po.y FROM po WHERE orr.a = po.x;" \
		"SELECT count(*), sum(b) FROM orr;" "Explicit Redistribute Motion"

	# A partitioned table's UPDATE or DELETE: a result relation for each
	# partition the plan scans, as the planner's are, found by the tableoid
	# ORCA's DML carries, each partition's columns by their names -- the
	# second partition has them in another order.
	q 0 "CREATE TABLE wp (a int, b int, c text) DISTRIBUTED BY (a) PARTITION BY RANGE (b);
	     CREATE TABLE wp_1 PARTITION OF wp FOR VALUES FROM (0) TO (10);
	     CREATE TABLE wp_2 (c text, b int, a int) DISTRIBUTED BY (a);
	     ALTER TABLE wp ATTACH PARTITION wp_2 FOR VALUES FROM (10) TO (20);
	     CREATE TABLE wp_3 PARTITION OF wp FOR VALUES FROM (20) TO (30);
	     INSERT INTO wp SELECT i, i % 30, 'c' || i FROM generate_series(1, 300) i; ANALYZE wp;" >/dev/null 2>&1
	orca_write "a partitioned table's UPDATE: the partitions it scans its result relations" \
		"UPDATE wp SET c = c || '!' WHERE b IN (5, 15);" \
		"SELECT tableoid::regclass, count(*) FROM wp WHERE c LIKE '%!' GROUP BY 1 ORDER BY 1;" \
		"Update on wp_1"
	orca_returning "a partitioned table's DELETE ... RETURNING, each row's partition" \
		"DELETE FROM wp WHERE a < 20 RETURNING tableoid::regclass, a, b, c;" \
		"Delete on wp_3"
	orca_returning "a partitioned table's UPDATE ... FROM another table, RETURNING old and new" \
		"UPDATE wp SET c = 'j' || po.y FROM po WHERE wp.a = po.x RETURNING wp.a, wp.b, new.c, old.c;" \
		"Update on wp_2"
	orca_write "a partitioned table's DELETE ... USING another table" \
		"DELETE FROM wp USING po WHERE wp.a = po.x AND po.y < 3;" \
		"SELECT tableoid::regclass, count(*) FROM wp GROUP BY 1 ORDER BY 1;" "Delete on wp"
	# A generic plan's parameter prunes a partitioned table's scan on the
	# segments, where the fragment is sent with the parameter's value and the
	# Dynamic Scan's steps: the planner's answers, a NULL among them.
	gen="SET plan_cache_mode = force_generic_plan; PREPARE wpp(int) AS SELECT count(*), sum(a) FROM wp WHERE b = \$1;"
	plan=$(printf '%s\n' "$gen" "EXPLAIN (COSTS OFF) EXECUTE wpp(15);" | qf 0)
	orca=$(printf '%s\n' "$gen" "EXECUTE wpp(15);" "EXECUTE wpp(25);" "EXECUTE wpp(NULL);" | qf 0)
	pg=$(printf '%s\n' "$gen" "SET gp.optimizer = off;" "EXECUTE wpp(15);" "EXECUTE wpp(25);" "EXECUTE wpp(NULL);" | qf 0)
	case "$plan" in
		*"Dynamic Seq Scan on wp"*"Optimizer: GPORCA"*)
			[ -n "$orca" ] && [ "$orca" = "$pg" ] \
				&& ok "a generic plan's parameter prunes a partitioned table's scan on the segments, answering as the planner" \
				|| notok "a generic plan's pruning on the segments" "ORCA: $orca / planner: $pg" ;;
		*) notok "a generic plan's pruning on the segments: the plan" "$plan" ;;
	esac
	orca_write "a partitioned table's UPDATE whose condition no partition's rows meet, and one joining the table to itself" \
		"UPDATE wp SET c = 'x' WHERE b = 100; UPDATE wp SET c = 'y' FROM wp w2 WHERE wp.a = w2.a AND w2.b = 5;" \
		"SELECT count(*) FILTER (WHERE c = 'x'), count(*) FILTER (WHERE c = 'y') FROM wp;" "Update on wp"
	# An UPDATE of a partitioned table's partition key or distribution key
	# moves rows: ORCA's Split, whose DELETE gp_core's node applies in the
	# partition the row's tableoid names, and whose INSERT it routes through
	# the table, as an INSERT into it is routed -- wp_2's columns in another
	# order -- the INSERT sent to the segment the new key hashes to.
	orca_write "a partitioned table's UPDATE of its partition key: each row deleted from its partition and routed to its new one" \
		"UPDATE wp SET b = (b + 12) % 30 WHERE a < 100;" \
		"SELECT tableoid::regclass, count(*), sum(a), sum(b), count(c) FROM wp GROUP BY 1 ORDER BY 1;" \
		"Split Update"
	orca_write "... and of its distribution key too, the rows moving to other segments and partitions" \
		"UPDATE wp SET a = a + 1000, b = 29 - b WHERE b < 15;" \
		"SELECT tableoid::regclass, gp_segment_id, count(*), sum(a), sum(b) FROM wp GROUP BY 1, 2 ORDER BY 1, 2;" \
		"Redistribute Motion 2:2"
	# A randomly distributed partitioned table: each row is written where it
	# is, and a row a join moved goes back to its segment by gp_segment_id,
	# ORCA's second column of the row's identity, its partition known by the
	# tableoid carried up from the scan that read it (gp_orca's
	# compat/wholerow.c) -- through a Split too, which passes it to both its
	# rows.
	q 0 "CREATE TABLE wr (a int, b int, c text) DISTRIBUTED RANDOMLY PARTITION BY RANGE (b);
	     CREATE TABLE wr_1 PARTITION OF wr FOR VALUES FROM (0) TO (10);
	     CREATE TABLE wr_2 PARTITION OF wr FOR VALUES FROM (10) TO (20);
	     INSERT INTO wr SELECT i, i % 20, 'c' || i FROM generate_series(1, 200) i; ANALYZE wr;" >/dev/null 2>&1
	wr_rows="SELECT tableoid::regclass, gp_segment_id, count(*), sum(a), sum(b), string_agg(DISTINCT left(c, 1), ',') FROM wr GROUP BY 1, 2 ORDER BY 1, 2;"
	orca_write "a randomly distributed partitioned table's UPDATE, each row written where it is" \
		"UPDATE wr SET c = 'x' || a WHERE a < 30;" "$wr_rows" "Update on wr_1"
	orca_write "... its DELETE" \
		"DELETE FROM wr WHERE a > 180;" "$wr_rows" "Delete on wr_2"
	orca_write "... its UPDATE joined to another table, each row routed back to its segment" \
		"UPDATE wr SET c = 'j' || po.y FROM po WHERE wr.a = po.x;" "$wr_rows" \
		"Explicit Redistribute Motion"
	orca_write "... and of its partition key, joined: a Split, both its rows routed back" \
		"UPDATE wr SET b = (wr.b + po.y) % 20 FROM po WHERE wr.a = po.x AND po.y < 4;" \
		"$wr_rows" "Split Update"

	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" "UPDATE wp SET b = b + 100 WHERE a = 7;" | qf 0)
	case "$out" in
		*"GPORCA failed"*) notok "a row no partition takes: planned by ORCA" "$out" ;;
		*'no partition of relation "wp" found for row'*)
			ok "... and a row no partition takes is refused, in PostgreSQL's words" ;;
		*) notok "a partitioned table's row routed nowhere" "$out" ;;
	esac

	# A subquery an UPDATE or DELETE compares with, finished in one place --
	# an aggregate, the first of its ordered rows: ORCA gathers to a segment,
	# where the slice that finishes it runs alone -- Cloudberry's singleton
	# reader -- and broadcasts the answer back to the writers.  The Gather's
	# senders stream every row to that one process, and a sorted Gather's
	# streams are merged there, each sender's apart, as they come; relayed,
	# they reach it through the coordinator, and are sorted there.
	orca_write "a DELETE of a subquery's aggregate: gathered to one segment, and broadcast back" \
		"DELETE FROM wu WHERE a = (SELECT max(x) FROM po WHERE x < 90);" \
		"SELECT count(*), sum(a) FROM wu;" "Gather Motion 2:1  (slice3; segments: 2)"
	orca_write "an UPDATE of the first of a subquery's ordered rows: a sorted Gather to one segment" \
		"UPDATE wu SET b = (SELECT x FROM po WHERE y = 2 ORDER BY x DESC LIMIT 1) WHERE a < 50;" \
		"SELECT count(*), sum(b) FROM wu;" "Merge Key"
	orca_write "... past an OFFSET, every sender's rows merged" \
		"UPDATE wu SET b = (SELECT x FROM po WHERE y = 2 ORDER BY x DESC OFFSET 7 LIMIT 1) WHERE a < 60;" \
		"SELECT count(*), sum(b) FROM wu;" "Merge Key"
	sql="DELETE FROM wu WHERE a = (SELECT max(x) FROM po WHERE x < 90);
		UPDATE wu SET b = (SELECT x FROM po WHERE y = 2 ORDER BY x DESC LIMIT 1) WHERE a < 50;
		UPDATE wu SET b = (SELECT x FROM po WHERE y = 2 ORDER BY x DESC OFFSET 7 LIMIT 1) WHERE a < 60;"
	want=$(printf '%s\n' "SET gp.optimizer = off;" "BEGIN;" "$sql" "SELECT count(*), sum(a), sum(b) FROM wu;" "ROLLBACK;" | qf 0)
	got=""
	for ic in udpifc relay; do
		got="$got$ic: $(printf '%s\n' "SET gp.interconnect_type = $ic;" "BEGIN;" "$sql" \
			"SELECT count(*), sum(a), sum(b) FROM wu;" "ROLLBACK;" | qf 0) "
	done
	[ -n "$want" ] && [ "$got" = "udpifc: $want relay: $want " ] \
		&& ok "... over UDP and relayed, the planner's answers" \
		|| notok "a Gather to one segment, over UDP and relayed" "$got / planner: $want"

	# A statement that fails on some segments while its slices stream fails
	# with their error, and ends: the rest of it is stopped as the first
	# fails (gp_dispatch.c) -- a slice whose receiver failed would wait for
	# it, over UDP, and its other receivers for the slice; and a reader that
	# starts after its writer failed would wait for the writer's snapshot.
	# Statements that fail so, one after another in a session, leave every
	# node up -- UDP's first packet of a slice no receiver has asked for yet
	# kept in the transaction's memory once crashed the segment at its end
	# (gp_ic.c).  Each interconnect, a statement timeout for a hang.
	crashes() { cat "$ROOT"/node*.log | grep -c "terminated by signal"; }
	before=$(crashes)
	q 0 "CREATE TABLE wck (a int, s text CHECK (length(s) < 5)) DISTRIBUTED RANDOMLY;" >/dev/null
	failed=""
	for ic in tcp udpifc relay; do
		out=$(printf '%s\n' "SET gp.interconnect_type = $ic;" "SET statement_timeout = '60s';" \
			"UPDATE wu SET b = (SELECT k FROM bo);" \
			"UPDATE wu SET b = 1 / (bo.k - 60) FROM bo WHERE wu.b = bo.k;" \
			"UPDATE wu SET b = (SELECT k FROM bo);" \
			"INSERT INTO wck VALUES (1, 'toolong');" "INSERT INTO wck VALUES (2, 'toolong');" \
			"SELECT 'alive';" | qf 0)
		case "$out" in
			*"more than one row returned by a subquery used as an expression"*"division by zero"*"more than one row returned by a subquery used as an expression"*"violates check constraint"*"violates check constraint"*alive) ;;
			*) failed="$failed $ic: $out" ;;
		esac
	done
	[ -z "$failed" ] && [ "$(crashes)" = "$before" ] \
		&& ok "statements that fail on the segments while their slices stream fail with the error, over tcp, UDP and relayed, and every node stays up" \
		|| notok "a statement failing on the segments" "$failed / crashes: $before before, $(crashes) after"

	# What Cloudberry refuses of a DO UPDATE, ORCA refuses in its words, as
	# the planner's route does, without falling back to it: a distribution
	# column set, and a volatile function in a replicated table's update.
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"INSERT INTO wu VALUES (5, 5, 'k') ON CONFLICT (a) DO UPDATE SET a = 500;" \
		"INSERT INTO wur VALUES (3, 3) ON CONFLICT (a) DO UPDATE SET b = random()::int;" | qf 0)
	case "$out" in
		*"falling back"*) notok "ON CONFLICT that Cloudberry refuses" "$out" ;;
		*"modification of distribution columns in OnConflictUpdate is not supported"*"modification of replicated tables containing volatile functions in OnConflictUpdate is not supported"*)
			ok "a DO UPDATE of the key, or volatile on a replicated table, is refused by ORCA, in Cloudberry's words" ;;
		*) notok "ON CONFLICT that Cloudberry refuses" "$out" ;;
	esac

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

	# A database made from template0 has gp_core's extension, which the
	# coordinator makes in it, and its segments' copies, as gpinitsystem
	# gives template1 and postgres theirs: ORCA's slices run there, which
	# gp_internal.exec_fragment() carries.
	q 0 "CREATE DATABASE db0 TEMPLATE template0;" >/dev/null
	out=$("$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d db0 \
		-c "SELECT count(*) FROM gp_dist_random('pg_extension') WHERE extname = 'gp_core';" \
		-c "CREATE TABLE z (a int, b int) DISTRIBUTED BY (a); INSERT INTO z SELECT i, i % 3 FROM generate_series(1, 30) i;" \
		-c "EXPLAIN (COSTS OFF) SELECT b, count(*) FROM z GROUP BY b;" \
		-c "SELECT string_agg(b || ':' || n, ',' ORDER BY b) FROM (SELECT b, count(*) n FROM z GROUP BY b) s;" 2>&1)
	case "$out" in
		2*"Redistribute Motion"*"Optimizer: GPORCA"*"0:10,1:10,2:10")
			ok "a database made from template0 has gp_core, on every node, and ORCA's Motions run there" ;;
		*) notok "gp_core in a database made from template0" "$out" ;;
	esac
	q 0 "DROP DATABASE db0;" >/dev/null

	# A segment applies a split update's DELETEs before its INSERTs, the
	# order ORCA sorts its rows into where the update changes a key of the
	# table's: an UPDATE that sets a unique key to another column that has
	# the same value deletes each old row before its new one is checked
	# against it.
	q 0 "CREATE TABLE wq (a int UNIQUE, b int) DISTRIBUTED BY (a); INSERT INTO wq SELECT i, i FROM generate_series(1, 50) i;" >/dev/null
	plan=$(q 0 "EXPLAIN (COSTS OFF) UPDATE wq SET a = b WHERE b <= 25;")
	tag=$("$PSQL" -X -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres \
		-c "UPDATE wq SET a = b WHERE b <= 25;" 2>&1)
	out=$(q 0 "SELECT count(*), sum(a) FROM wq;")
	case "$plan" in
		*"Split Update"*)
			[ "$tag|$out" = "UPDATE 25|50|1275" ] \
				&& ok "a split update of a unique key to the value it has: each row's DELETE before its INSERT" \
				|| notok "a split update of a unique key" "$tag / $out" ;;
		*) notok "a split update of a unique key: the plan" "$plan" ;;
	esac

	# A table whose triggers are none of them UPDATE's has none an UPDATE
	# fires: its key's UPDATE is ORCA's Split, which fires none of them --
	# here each would fail the statement -- the rows moved as the planner's
	# route moves them.
	q 0 "CREATE TABLE wt (a int, b int) DISTRIBUTED BY (a); INSERT INTO wt SELECT i, i FROM generate_series(1, 20) i;" >/dev/null
	q 0 "CREATE FUNCTION wt_fail() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RAISE EXCEPTION 'fired'; END \$\$;" >/dev/null
	q 0 "CREATE TRIGGER wt_t BEFORE INSERT OR DELETE ON wt FOR EACH ROW EXECUTE FUNCTION wt_fail();" >/dev/null
	orca_write "an UPDATE of the key of a table whose triggers are INSERT's and DELETE's: a Split, firing none, as an UPDATE fires none" \
		"UPDATE wt SET a = a + 100 WHERE b < 10;" \
		"SELECT gp_segment_id, count(*), sum(a) FROM wt GROUP BY 1 ORDER BY 1;" "Split Update"

	# The planner's Split, where the cluster has its secret: the segments'
	# split functions move each row, firing no INSERT or DELETE trigger, as
	# Cloudberry's Split fires none; a row's own INSERT and DELETE fire them
	# (gp_split.c).
	q 0 "CREATE TABLE wtn (a int, b text, c int GENERATED ALWAYS AS (a * 2) STORED) DISTRIBUTED BY (a); CREATE INDEX ON wtn (b);" >/dev/null
	q 0 "CREATE FUNCTION wtn_say() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RAISE NOTICE 'fired % %', TG_WHEN, TG_OP; IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END \$\$;" >/dev/null
	q 0 "CREATE TRIGGER wtn_bi BEFORE INSERT ON wtn FOR EACH ROW EXECUTE FUNCTION wtn_say(); CREATE TRIGGER wtn_ad AFTER DELETE ON wtn FOR EACH ROW EXECUTE FUNCTION wtn_say();" >/dev/null
	out=$(printf '%s\n' "SET gp.optimizer = off;" "INSERT INTO wtn SELECT g, 'v' || g FROM generate_series(1, 20) g;" \
		"UPDATE wtn SET a = a + 100 WHERE a <= 10 RETURNING old.a, new.a, new.c;" \
		"DELETE FROM wtn WHERE a = 101;" | qf 0)
	n=$(printf '%s\n' "$out" | grep -c 'NOTICE:  fired')
	n2=$(printf '%s\n' "$out" | grep -c '^[0-9]*|1[0-9][0-9]|2[0-9][0-9]$')
	out2=$(q 0 "SELECT count(*), sum(a), sum(c) FROM wtn;")
	i1=$(q 1 "SET enable_seqscan = off; SELECT count(*) - (SELECT count(*) FROM wtn) FROM wtn WHERE b > '';")
	i2=$(q 2 "SET enable_seqscan = off; SELECT count(*) - (SELECT count(*) FROM wtn) FROM wtn WHERE b > '';")
	[ "$n|$n2|$out2|$(placed wtn)|$i1|$i2" = "21|10|19|1109|2218|0|0|0|0" ] \
		&& ok "the planner's Split fires no INSERT or DELETE trigger, as Cloudberry's fires none; each row where it hashes, its index entry and generated column made" \
		|| notok "the planner's Split and triggers" "$n $n2 / $out2 / misplaced $(placed wtn) / index $i1 $i2 / $out"
	out=$(q 1 "SELECT * FROM gp_internal.split_delete(NULL::wtn, ARRAY[]::tid[], ARRAY[]::oid[], ARRAY[]::int8[]);")
	case "$out" in
		*"gp_internal.split_delete() moves rows only for the coordinator"*) ok "... and a segment moves rows so only for the coordinator" ;;
		*) notok "split_delete() called directly" "$out" ;;
	esac
	# ... and MERGE's, as Cloudberry's SplitMerge: a row an UPDATE action
	# moves fires nothing, a MATCHED DELETE and a NOT MATCHED INSERT fire
	# their triggers.
	out=$(printf '%s\n' "SET gp.optimizer = off;" \
		"MERGE INTO wtn t USING (VALUES (11, 'x'), (12, NULL), (300, 'y')) s(a, b) ON t.a = s.a WHEN MATCHED AND s.b IS NULL THEN DELETE WHEN MATCHED THEN UPDATE SET a = t.a + 1000, b = s.b WHEN NOT MATCHED THEN INSERT VALUES (s.a, s.b);" | qf 0)
	n=$(printf '%s\n' "$out" | grep -c 'NOTICE:  fired')
	out2=$(q 0 "SELECT count(*), sum(a), sum(c) FROM wtn;")
	[ "$n|$out2|$(placed wtn)" = "2|19|2397|4794|0|0" ] \
		&& ok "MERGE's Split fires no trigger; its DELETE and INSERT fire theirs; each row where it hashes" \
		|| notok "MERGE's Split and triggers" "$n / $out2 / misplaced $(placed wtn) / $out"

	# EXPLAIN ANALYZE CREATE TABLE AS: the table made on every node and
	# filled by an INSERT, the one explained -- ORCA's plan of it -- where
	# PostgreSQL's would fill a table on the coordinator alone (gp_sql.c).
	want=$(q 0 "SELECT count(*) FROM o WHERE a <= 100;")
	out=$(q 0 "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) CREATE TABLE xct AS SELECT * FROM o WHERE a <= 100 DISTRIBUTED BY (b);")
	out2=$(q 0 "SELECT count(*), (gp.policy('xct')).columns FROM xct;")
	n1=$(q 1 "SELECT count(*) FROM xct;"); n2=$(q 2 "SELECT count(*) FROM xct;")
	case "$out|$out2" in
		*"Insert on xct"*"Optimizer: GPORCA"*"|$want|{b}")
			isnum "$n1" && isnum "$n2" && [ "$((n1 + n2))" = "$want" ] \
				&& ok "EXPLAIN ANALYZE CREATE TABLE AS: the table on every node, filled by the INSERT ORCA plans and EXPLAIN shows" \
				|| notok "EXPLAIN ANALYZE CREATE TABLE AS: the segments' rows" "$n1 $n2" ;;
		*) notok "EXPLAIN ANALYZE CREATE TABLE AS" "$out / $out2" ;;
	esac

	# A streaming slice's rows travel as tuples, as Cloudberry's do: a value
	# TOAST keeps out of line brought in, a composite, jsonb, an array
	# (gp_motion.c).  The same rows as the planner's, which gathers them.
	q 0 "CREATE TYPE tpair AS (x int, y text); CREATE TABLE tup1 (a int, b int, big text, j jsonb, arr int[], p tpair) DISTRIBUTED BY (a); ALTER TABLE tup1 ALTER COLUMN big SET STORAGE EXTERNAL;" >/dev/null
	q 0 "INSERT INTO tup1 SELECT g, g % 13, CASE WHEN g % 10 = 0 THEN repeat(md5(g::text), 400) END, jsonb_build_object('k', g), ARRAY[g, g + 1], ROW(g, 'p' || g)::tpair FROM generate_series(1, 3000) g; ANALYZE tup1;" >/dev/null
	sel="SELECT x.a, y.a AS ya, x.big, x.j, x.arr, x.p, y.big AS ybig, y.p AS yp FROM tup1 x JOIN tup1 y ON x.a = y.b"
	plan=$(q 0 "EXPLAIN (COSTS OFF) $sel;")
	out=$(printf '%s\n' "SET gp.optimizer = on;" "CREATE TEMP TABLE tup_o AS $sel;" "SET gp.optimizer = off;" \
		"CREATE TEMP TABLE tup_p AS $sel;" \
		"SELECT (SELECT count(*) FROM tup_o), (SELECT count(*) FROM (SELECT a, ya, md5(big), j::text, arr, p::text, md5(ybig), yp::text FROM tup_o EXCEPT ALL SELECT a, ya, md5(big), j::text, arr, p::text, md5(ybig), yp::text FROM tup_p) d), (SELECT count(*) FROM (SELECT a, ya, md5(big), j::text, arr, p::text, md5(ybig), yp::text FROM tup_p EXCEPT ALL SELECT a, ya, md5(big), j::text, arr, p::text, md5(ybig), yp::text FROM tup_o) d);" | qf 0 | tail -1)
	case "$plan|$out" in
		*"Motion 2:2"*"Optimizer: GPORCA"*"|2770|0|0")
			ok "a streaming slice's rows as tuples: TOAST's values, a composite, jsonb, an array; the planner's rows" ;;
		*) notok "rows as tuples" "$out / $plan" ;;
	esac

	# A partial table is the planner's, as Cloudberry's ORCA leaves one.
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"SELECT count(*) FROM pt1;" | qf 0)
	case "$out" in
		*"Partially Distributed Data"*"100") ok "ORCA leaves a partial table to the planner, and says why" ;;
		*) notok "a partial table under ORCA" "$out" ;;
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

	# EXPLAIN ANALYZE describes a fragment the coordinator never runs, with
	# the segments' figures; an index scan in it, whose count of searches
	# once stopped the coordinator (qp_join_union_all), counts theirs.  A
	# column the index does not hold, so that the scan is not an index-only
	# one, which ORCA chooses now that it knows the segments' all-visible
	# pages.
	q 0 "CREATE INDEX o_b ON o (b); ANALYZE o;" >/dev/null
	out=$(printf '%s\n' "SET enable_seqscan = off;" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(c) FROM o WHERE b = 3;" | qf 0)
	case "$out" in
		*"Index Scan using o_b on o (actual rows="*"Index Searches: 2"*) ok "EXPLAIN ANALYZE of an index scan in a fragment: the segments' searches, one each" ;;
		*) notok "EXPLAIN ANALYZE of an index scan in a fragment" "$out" ;;
	esac
	q 0 "DROP INDEX o_b;" >/dev/null

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

	# A node tree and extended statistics refuse to be read back by input or
	# receive; they travel as text and bytea, whose bytes they are.
	q 0 "CREATE TABLE ntp (a int, b int) DISTRIBUTED BY (a) PARTITION BY RANGE (b) (START (1) END (3) EVERY (1));" >/dev/null
	out=$(q 0 "SELECT count(*), count(DISTINCT relpartbound::text), count(DISTINCT gp_segment_id) FROM gp_dist_random('pg_class') WHERE relname LIKE 'ntp_1_prt%';")
	q 0 "CREATE TABLE nts (a int, b int) DISTRIBUTED BY (a); INSERT INTO nts SELECT i % 10, i % 10 FROM generate_series(1, 1000) i; CREATE STATISTICS nts_s (ndistinct, mcv) ON a, b FROM nts;" >/dev/null
	q 1 "ANALYZE nts;" >/dev/null
	out2=$(q 0 "SELECT s.gp_segment_id, octet_length(stxdndistinct::bytea) > 0, octet_length(stxdmcv::bytea) > 0 FROM gp_dist_random('pg_statistic_ext_data') s JOIN pg_statistic_ext e ON e.oid = s.stxoid WHERE e.stxname = 'nts_s';")
	[ "$out|$out2" = "4|2|2|0|t|t" ] \
		&& ok "gp_dist_random() of a catalog reads its node trees and its statistics' values" \
		|| notok "pg_node_tree and pg_ndistinct through gp_dist_random()" "$out / $out2"

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

	# And writes one only where gp.allow_segment_dml is on, Cloudberry's
	# allow_segment_DML, which the segments are sent: a function an index
	# calls, as each segment inserts its copy's row (the trap Cloudberry's
	# privileges test lays for CVE-2020-25695), writes each segment's copy.
	q 0 "CREATE TABLE lw (s text) DISTRIBUTED REPLICATED;
		CREATE TABLE lx (a int) DISTRIBUTED REPLICATED;
		CREATE FUNCTION lx_f(int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT \$1';
		CREATE INDEX lx_i ON lx (lx_f(a));
		CREATE OR REPLACE FUNCTION lx_f(int) RETURNS int LANGUAGE sql AS 'INSERT INTO lw VALUES (current_user); SELECT \$1';" >/dev/null
	nseg=$(q 0 "SELECT count(*) FROM gp_dist_random('gp_id');")
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" "INSERT INTO lx VALUES (1);" \
		"SET gp.allow_segment_dml = on;" "INSERT INTO lx VALUES (2);" \
		"SELECT count(*) FROM gp_dist_random('lw');" | qf 0)
	case "$out" in
		*"fallback"*) notok "gp.allow_segment_dml and a function writing in a fragment" "$out" ;;
		*"function cannot execute on a QE slice because it issues a non-SELECT statement"*)
			[ "${out##*$'\n'}" = "$nseg" ] \
				&& ok "a function in a fragment writes a replicated table only where gp.allow_segment_dml is on" \
				|| notok "gp.allow_segment_dml and a function writing in a fragment" "$out" ;;
		*) notok "gp.allow_segment_dml and a function writing in a fragment" "$out" ;;
	esac
	q 0 "DROP TABLE lx, lw; DROP FUNCTION lx_f(int);" >/dev/null

	# Cloudberry's runtime filters (gp_rtfilter.c).  With
	# gp.enable_runtime_filter on, a hash join of the planner's whose inner
	# side meets few of its outer rows has a RuntimeFilter above its outer
	# side: the inner rows' hash values in a Bloom filter, which passes on
	# only the outer rows that may meet one, and a row with a NULL key.  With
	# gp.enable_runtime_filter_pushdown on, an integer key's inner values and
	# range reach the scans below the outer side -- the planner's gathers,
	# which send their segments the range, and on the segments the
	# sequential scans of ORCA's slices -- which test a row as they read it,
	# before their own conditions, and EXPLAIN ANALYZE says how many rows
	# each dropped, first of the node's lines, as Cloudberry's does.  A left
	# join's preserved side gets neither, and every answer is the one
	# without.
	q 0 "CREATE TABLE rff (id int, d int) DISTRIBUTED BY (id);
	     INSERT INTO rff SELECT i, CASE WHEN i % 400 = 0 THEN NULL ELSE i % 2000 END FROM generate_series(1, 40000) i;
	     CREATE TABLE rfd (d int, p int) DISTRIBUTED BY (d);
	     INSERT INTO rfd SELECT i, i % 10 FROM generate_series(0, 1999) i;
	     ANALYZE rff; ANALYZE rfd;" >/dev/null
	rf_join="SELECT count(*), count(DISTINCT d) FROM rff JOIN rfd USING (d) WHERE p = 0;"
	rf_left="SELECT count(*), count(p) FROM rff LEFT JOIN (SELECT * FROM rfd WHERE p = 0) f USING (d);"
	rf_explain="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)"
	out=$(printf '%s\n' "SET gp.optimizer = off;" "SET gp.enable_runtime_filter = on;" \
		"$rf_explain $rf_join" "$rf_join" "SET gp.enable_runtime_filter = off;" "$rf_join" | qf 0 | tr '\n' '|')
	case "$out" in
		*"->  RuntimeFilter (actual rows=4000.00 loops=1)|"*"Bloom Bits: 1048576|"*"Extra Text: Inner Processed: 200, Flase Positive Rate: 0.0"*"->  Gather Motion 2:1 on rff  (slice1; segments: 2) (actual rows=40000.00 loops=1)|"*"|3900|195|3900|195|")
			ok "the planner's hash join takes a RuntimeFilter: 40,000 outer rows, 4,000 passed -- 3,900 that meet, 100 with a NULL key -- the 200 inner rows it holds in Cloudberry's Extra Text, and the same answer" ;;
		*) notok "a RuntimeFilter on the planner's route" "$out" ;;
	esac
	out=$(printf '%s\n' "SET gp.optimizer = off;" "SET gp.enable_runtime_filter_pushdown = on;" \
		"${rf_explain%)}, VERBOSE) $rf_join" "$rf_join" | qf 0 | tr '\n' '|')
	case "$out" in
		*"RuntimeFilter"*) notok "pushdown alone" "$out" ;;
		*"->  Gather Motion 2:1 on public.rff  (slice1; segments: 2) (actual rows=3900.00 loops=1)|"*"Rows Removed by Pushdown Runtime Filter: 35820|"*"Remote SQL: SELECT d FROM ONLY public.rff WHERE (d >= '0'::integer AND d <= '1990'::integer)|"*"|3900|195|")
			ok "pushed down, the gather sends its segments the key's range, where 280 rows stay, 100 of them with a NULL key, and the key's values drop 35,820 where it reads them, which EXPLAIN ANALYZE says" ;;
		*) notok "pushdown into the planner's gather" "$out" ;;
	esac
	# A gather tests a row as it reads it, before its own conditions: the
	# count is of all it read, "Rows Removed by Filter" of what passed.  A
	# cursor's gathers start as it is declared, before any hash table, and
	# send no range.
	q 0 "CREATE FUNCTION rf_even(int) RETURNS bool LANGUAGE plpgsql STABLE AS 'BEGIN RETURN \$1 % 2 = 0; END';" >/dev/null
	rf_local="SELECT count(*) FROM rff JOIN rfd USING (d) WHERE p = 0 AND rf_even(rff.id / 10);"
	out=$(printf '%s\n' "SET gp.optimizer = off;" "SET gp.enable_runtime_filter_pushdown = on;" \
		"$rf_explain $rf_local" "$rf_local" "BEGIN;" "DECLARE rfc CURSOR FOR $rf_join" "FETCH ALL FROM rfc;" "COMMIT;" \
		"SET gp.enable_runtime_filter_pushdown = off;" "$rf_local" | qf 0 | tr '\n' '|')
	sp=$(printf '%14s' '')
	case "$out" in
		*"->  Gather Motion 2:1 on rff  (slice1; segments: 2) (actual rows=1900.00 loops=1)|${sp}Rows Removed by Pushdown Runtime Filter: 35820|${sp}Filter: rf_even((id / 10))|${sp}Rows Removed by Filter: 2000|"*"|1900|3900|195|1900|")
			ok "a gather drops what the filter rules out before its own condition, and says so first, as Cloudberry's scan does; a cursor's gathers answer as without" ;;
		*) notok "a gather's count before its own condition" "$out" ;;
	esac
	out=$(printf '%s\n' "SET gp.optimizer = off;" "SET gp.enable_runtime_filter = on;" \
		"SET gp.enable_runtime_filter_pushdown = on;" "$rf_explain $rf_left" "$rf_left" | qf 0 | tr '\n' '|')
	case "$out" in
		*"RuntimeFilter"*|*"Pushdown Runtime Filter"*) notok "a left join's preserved side, filtered" "$out" ;;
		*"Hash Left Join"*"|40000|3900|") ok "a left join's preserved side is filtered by neither" ;;
		*) notok "a left join under the runtime filters" "$out" ;;
	esac
	# A hash join run for each row of a subquery's, its hash table made again
	# for each: its filters are the table's that it probes, each time.
	out=$(for on in on off; do
		printf '%s\n' "SET gp.optimizer = off;" "SET gp.enable_runtime_filter = $on;" \
			"SET gp.enable_runtime_filter_pushdown = $on;" \
			"SELECT x.p, (SELECT count(*) FROM rff JOIN rfd USING (d) WHERE rfd.p = x.p) FROM (VALUES (0), (3), (7)) x(p) ORDER BY 1;" | qf 0
	done | tr '\n' ' ')
	[ "$out" = "0|3900 3|4000 7|4000 0|3900 3|4000 7|4000 " ] \
		&& ok "a hash join whose hash table is made again for each row keeps its answers" \
		|| notok "a filtered hash join made again for each row" "$out"
	# A join's outer side grouped by its key -- the planner's own, and its
	# unique path for a semi-join -- is filtered below the grouping, which
	# only drops groups no inner row meets; not where the join runs again
	# and a HashAggregate gives it again what it made under another hash
	# table.
	rf_grouped="SELECT count(*) FROM (SELECT DISTINCT d FROM rff) f JOIN rfd USING (d) WHERE p = 0;"
	rf_regrouped="SELECT x.p, (SELECT count(*) FROM (SELECT DISTINCT d FROM rff) f JOIN rfd USING (d) WHERE rfd.p = x.p) FROM (VALUES (0), (3), (7)) x(p) ORDER BY 1;"
	out=$(printf '%s\n' "SET gp.optimizer = off;" "SET gp.enable_runtime_filter = on;" \
		"$rf_explain $rf_grouped" "$rf_grouped" | qf 0 | tr '\n' '|')
	regrouped=$(for on in on off; do
		printf '%s\n' "SET gp.optimizer = off;" "SET enable_mergejoin = off;" "SET enable_nestloop = off;" \
			"SET gp.enable_runtime_filter = $on;" "SET gp.enable_runtime_filter_pushdown = $on;" \
			"$rf_regrouped" | qf 0
	done | tr '\n' ' ')
	case "$out|$regrouped" in
		*"->  HashAggregate (actual rows=196.00 loops=1)|"*"->  RuntimeFilter (actual rows=4000.00 loops=1)|"*"|195||0|195 3|200 7|200 0|195 3|200 7|200 ")
			ok "a join's outer side grouped by its key is filtered below the grouping, 4,000 of 40,000 rows grouped, and where the join runs again it keeps its answers" ;;
		*) notok "a filter below a grouping node" "$out / $regrouped" ;;
	esac
	# ORCA's hash joins run on the segments, which are sent the setting, and
	# its scans there drop what the key rules out, as a segment's own plan
	# shows; the coordinator's EXPLAIN ANALYZE describes ORCA's fragment
	# without running it.
	out=$(for on in on off; do
		printf '%s\n' "SET gp.enable_runtime_filter_pushdown = $on;" "$rf_join" "$rf_left" | qf 0
	done | tr '\n' ' ')
	plan=$(q 0 "EXPLAIN (COSTS OFF) $rf_join")
	seg=$(q 0 "SET gp.enable_runtime_filter_pushdown = on; SELECT string_agg(DISTINCT current_setting('gp.enable_runtime_filter_pushdown'), ',') FROM gp_dist_random('gp_id');")
	case "$plan|$out|$seg" in
		*"Gather Motion"*"Hash Join"*"Optimizer: GPORCA|3900|195 40000|3900 3900|195 40000|3900 |on")
			ok "ORCA's hash joins run on the segments, sent the setting, and answer as without it" ;;
		*) notok "pushdown under ORCA" "$plan / $out / $seg" ;;
	esac
	out=$(printf '%s\n' "SET gp.enable_runtime_filter_pushdown = on;" "SET enable_mergejoin = off;" \
		"SET enable_nestloop = off;" "$rf_explain $rf_join" | qf 1 | tr '\n' '|')
	case "$out" in
		*"->  Seq Scan on rff (actual rows="*"|"*"Rows Removed by Pushdown Runtime Filter: "[1-9]*) ok "a segment's sequential scan drops the rows the key rules out" ;;
		*) notok "pushdown into a segment's sequential scan" "$out" ;;
	esac
	# ORCA's: a segment's scan tests a row as its table gives it, before the
	# scan's own condition, and the coordinator's EXPLAIN ANALYZE says what
	# the segment with the most rows said -- 0 too, where the filter worked
	# and dropped nothing, as Cloudberry prints it (prf_work).
	q 0 "CREATE TABLE rfr (d int) DISTRIBUTED REPLICATED;
	     INSERT INTO rfr SELECT generate_series(0, 1999); ANALYZE rfr;" >/dev/null
	rf_orca="SELECT count(*) FROM rff JOIN rfd USING (d) WHERE p = 0 AND (rff.id / 10) % 2 = 0;"
	out=$(for on in on off; do
		printf '%s\n' "SET gp.enable_runtime_filter_pushdown = $on;" "$rf_orca" | qf 0
	done | tr '\n' ' ')
	plan=$(printf '%s\n' "SET gp.enable_runtime_filter_pushdown = on;" "$rf_explain $rf_orca" \
		"$rf_explain SELECT count(*) FROM rff JOIN rfr USING (d);" | qf 0 | tr '\n' '|')
	sp=$(printf '%26s' '')
	case "$out|$plan" in
		"1900 1900 |"*"->  Seq Scan on rff (actual rows=1005.00 loops=1)|${sp}Rows Removed by Pushdown Runtime Filter: 17942|${sp}Filter: (((id / 10) % 2) = 0)|${sp}Rows Removed by Filter: 990|"*"->  Seq Scan on rff (actual rows=20063.00 loops=1)|${sp}Rows Removed by Pushdown Runtime Filter: 0|"*)
			ok "a segment's scan under ORCA counts the rows it drops before its own condition, first of its lines, and 0 where it drops none" ;;
		*) notok "a segment's count under ORCA" "$out / $plan" ;;
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

	# EXPLAIN (SLICETABLE) of ORCA's plan is the slice table it carries --
	# the Redistribute's slice under the Gather's -- and EXPLAIN (LOCUS)
	# prints nothing under ORCA, as Cloudberry's (gp_explain.c).
	out=$(q 0 "EXPLAIN (SLICETABLE, LOCUS, COSTS OFF) SELECT count(*) FROM o JOIN po ON o.a = po.y;")
	case "$out" in
		*"Locus:"*) notok "EXPLAIN (LOCUS) under ORCA" "$out" ;;
		*"Slice 0: Dispatcher; root 0; parent -1; gang size 0"*"Reader; root 0; parent 1; gang size 2"*"Optimizer: GPORCA"*)
			ok "EXPLAIN (SLICETABLE) prints ORCA's slices, and (LOCUS) nothing under ORCA" ;;
		*) notok "EXPLAIN (SLICETABLE) of ORCA's plan" "$out" ;;
	esac

	# The planner's route sends a scan's stable conditions to the segments --
	# the built-in functions that are stable for the settings a statement is
	# sent with, or for the transaction's start, which the gather brings --
	# as Cloudberry's segments evaluate them; what a node answers of itself
	# stays here.  Their answers are the coordinator's, in any time zone.
	q 0 "CREATE TABLE stc (id int, ts timestamptz, amt numeric, body text) DISTRIBUTED BY (id);
	     INSERT INTO stc SELECT i, '2026-09-20 00:00:00+00'::timestamptz + (i * 37 || ' minutes')::interval,
	                           i * 1.5, 'the quick brown fox ' || i FROM generate_series(1, 400) i;" >/dev/null
	out=$(q 0 "SET gp.optimizer = off; SET TimeZone = 'Asia/Tokyo';
		EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM stc WHERE ts > now() - interval '9 days'
		   AND ts::date < '2026-09-27'::date AND amt::money > '10'::money AND to_tsvector(body) @@ to_tsquery('fox')
		   AND current_setting('TimeZone') <> '' AND id <> pg_backend_pid();" | grep -E "Remote SQL|Filter")
	same=0
	for tz in UTC Asia/Tokyo America/New_York; do
		sent=$(q 0 "SET gp.optimizer = off; SET TimeZone = '$tz';
			SELECT count(*) FROM stc WHERE extract(hour FROM ts) < 12 AND ts::date = '2026-09-22'::date;")
		here=$(q 0 "SET gp.optimizer = off; SET TimeZone = '$tz';
			SELECT count(*) FROM (SELECT * FROM stc OFFSET 0) s WHERE extract(hour FROM ts) < 12 AND ts::date = '2026-09-22'::date;")
		[ -n "$sent" ] && [ "$sent" = "$here" ] && same=$((same + 1))
	done
	sql=$(printf '%s\n' "$out" | sed -n 's/^ *Remote SQL: //p')
	sent=1
	for want in "now()" "::date" "::money" "to_tsvector(body)"; do
		case "$sql" in *"$want"*) ;; *) sent=0 ;; esac
	done
	case "$sql" in *current_setting*|*pg_backend_pid*) sent=0 ;; esac
	[ "$(printf '%s\n' "$out" | grep -c 'pg_backend_pid\|current_setting')" = 2 ] || sent=0
	[ "$same" = 3 ] && [ "$sent" = 1 ] \
		&& ok "under the planner a scan's stable conditions go to the segments -- now(), a date against a time with a zone, a cast to money, to_tsvector() -- and answer as here in three time zones; current_setting() and pg_backend_pid() stay here" \
		|| notok "a scan's stable conditions under the planner, as here in three time zones" "$same / $out"
	out=$(printf '%s\n' "SET gp.optimizer = off;" "BEGIN;" \
		"CREATE TABLE stn (id int, ts timestamptz) DISTRIBUTED BY (id);" \
		"INSERT INTO stn SELECT i, now() FROM generate_series(1, 30) i;" \
		"SELECT pg_sleep(1.1);" "SELECT count(*) FROM stn WHERE ts = now() AND ts = CURRENT_TIMESTAMP;" \
		"COMMIT;" | qf 0 | tail -1)
	[ "$out" = 30 ] \
		&& ok "and now() and CURRENT_TIMESTAMP on the segments are the coordinator's transaction start" \
		|| notok "now() in a condition sent to the segments" "$out"

	# A correlated scalar subquery of an aggregate, which the planner would
	# run for each row -- a gather of the table at each -- is a join with its
	# rows grouped by the correlation, as Cloudberry's planner makes it
	# (gp_subselect.c): TPC-H's query 20.  Where the subquery's value over
	# no rows is not NULL -- count() -- it stays a subquery.  The answers are
	# the subquery's own, which OFFSET 0 keeps one.
	q 0 "CREATE TABLE cps (pk int, sk int, qty int) DISTRIBUTED BY (pk);
	     CREATE TABLE cli (pk int, sk int, n numeric, d date) DISTRIBUTED BY (pk);
	     INSERT INTO cps SELECT p, s, (p * 7 + s * 13) % 100 FROM generate_series(1, 200) p, generate_series(1, 4) s;
	     INSERT INTO cli SELECT (i % 250) + 1, (i / 250) % 4 + 1, i % 3, date '1993-06-01' + (i % 900) FROM generate_series(1, 20000) i;
	     ANALYZE cps; ANALYZE cli;" >/dev/null
	sub="SELECT 0.5 * sum(n) FROM cli WHERE cli.pk = cps.pk AND cli.sk = cps.sk AND d >= date '1994-01-01' AND d < date '1995-01-01'"
	plan=$(q 0 "SET gp.optimizer = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM cps WHERE qty > ($sub);" | tr '\n' ' ')
	joined=$(q 0 "SET gp.optimizer = off; SELECT count(*), sum(qty) FROM cps WHERE qty > ($sub);")
	perrow=$(q 0 "SET gp.optimizer = off; SELECT count(*), sum(qty) FROM cps WHERE qty > ($sub OFFSET 0);")
	inner=$(q 0 "SET gp.optimizer = off; SELECT count(*) FROM cps p1 WHERE p1.sk IN (SELECT sk FROM cps WHERE pk < 50 AND (SELECT max(n) FROM cli WHERE cli.pk = cps.pk) < qty);")
	inner1=$(q 0 "SET gp.optimizer = off; SELECT count(*) FROM cps p1 WHERE p1.sk IN (SELECT sk FROM cps WHERE pk < 50 AND (SELECT max(n) FROM cli WHERE cli.pk = cps.pk OFFSET 0) < qty);")
	counted=$(q 0 "SET gp.optimizer = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM cps WHERE qty > (SELECT count(*) FROM cli WHERE cli.pk = cps.pk);" | grep -c SubPlan)
	case "$plan" in
		*SubPlan*) notok "a correlated scalar subquery of an aggregate as a join" "$plan" ;;
		*"Group Key: cli.pk, cli.sk"*)
			[ "$joined" = "$perrow" ] && [ -n "$joined" ] && [ "$inner" = "$inner1" ] && [ "$counted" -ge 1 ] \
				&& ok "under the planner a correlated scalar subquery of an aggregate is a join with its rows grouped by the correlation, answering as the subquery does; count()'s stays a subquery" \
				|| notok "a correlated scalar subquery of an aggregate as a join" "$joined / $perrow / $inner / $inner1 / $counted" ;;
		*) notok "a correlated scalar subquery of an aggregate as a join" "$plan" ;;
	esac

	# A write of a materialized view is refused as PostgreSQL's
	# CheckValidResultRel() refuses it, before a row is made, in its words and
	# on the coordinator -- where the segments' COPY refused the routed rows in
	# its own, "cannot copy to materialized view" -- under EXPLAIN too, and
	# an unpopulated view's as well; a read of an unpopulated view by a user
	# who may not read it is refused for that first, as InitPlan() checks.
	# REFRESH, which writes the view under its maintenance, still fills it.
	# Here, where the coordinator has the secret a segment fills a view by.
	# mvw_writes <settings> <filter>: what each write says, one after another
	mvw_writes() {
		local stmt
		for stmt in "INSERT INTO mvw VALUES (2, 2)" "INSERT INTO mvw SELECT * FROM mvw_base" \
				"INSERT INTO mvw VALUES (2, 2) RETURNING *" "UPDATE mvw SET b = 3" "DELETE FROM mvw" \
				"UPDATE mvw SET b = 3 FROM mvw_base WHERE mvw.a = mvw_base.a" \
				"EXPLAIN INSERT INTO mvw VALUES (2, 2)" "INSERT INTO mvw_rep VALUES (2, 2)" \
				"INSERT INTO mvw_empty VALUES (2, 2)" "INSERT INTO mvw_empty SELECT * FROM mvw_empty"; do
			q 0 "$1 $stmt;" | $2
		done
	}
	mvw_want='ERROR:  cannot change materialized view "mvw"
ERROR:  cannot change materialized view "mvw"
ERROR:  cannot change materialized view "mvw"
ERROR:  cannot change materialized view "mvw"
ERROR:  cannot change materialized view "mvw"
ERROR:  cannot change materialized view "mvw"
ERROR:  cannot change materialized view "mvw"
ERROR:  cannot change materialized view "mvw_rep"
ERROR:  cannot change materialized view "mvw_empty"
ERROR:  cannot change materialized view "mvw_empty"'
	setup=$(q 0 "CREATE TABLE mvw_base (a int, b int) DISTRIBUTED BY (a); INSERT INTO mvw_base VALUES (1, 1);"
			q 0 "CREATE MATERIALIZED VIEW mvw AS SELECT a, b FROM mvw_base DISTRIBUTED BY (a);"
			q 0 "CREATE MATERIALIZED VIEW mvw_rep AS SELECT a, b FROM mvw_base DISTRIBUTED REPLICATED;"
			q 0 "CREATE MATERIALIZED VIEW mvw_empty AS SELECT a, b FROM mvw_base WITH NO DATA DISTRIBUTED BY (a);"
			q 0 "CREATE ROLE mvw_reader LOGIN;")
	out=$(mvw_writes "SET gp.optimizer = off;" cat)
	[ "$out" = "$mvw_want" ] && ok "a write of a materialized view is refused in PostgreSQL's words, on the coordinator" \
		|| notok "a write of a materialized view" "$setup / $out"
	out=$(q 0 "SET ROLE mvw_reader; SELECT * FROM mvw_empty;")
	out2=$(q 0 "SELECT * FROM mvw_empty;" | head -1)
	q 0 "INSERT INTO mvw_base VALUES (2, 2);" >/dev/null
	out3=$(q 0 "REFRESH MATERIALIZED VIEW mvw;" && q 0 "SELECT count(*) FROM mvw;")
	[ "$out|$out2|$out3" = 'ERROR:  permission denied for materialized view mvw_empty|ERROR:  materialized view "mvw_empty" has not been populated|2' ] \
		&& ok "... a read of an unpopulated one checks the privileges first, and REFRESH still writes one" \
		|| notok "an unpopulated materialized view, and REFRESH" "$out / $out2 / $out3"

	# Under ORCA in the same words, which a segment's ModifyTable says where
	# ORCA writes there -- an unpopulated view's too, whose write is no scan
	# of it.
	out=$(mvw_writes "SET gp.optimizer = on;" "head -1")
	[ "$out" = "$mvw_want" ] && ok "under ORCA a write of a materialized view is refused in the same words" \
		|| notok "a write of a materialized view under ORCA" "$out"

	# NOT IN of a subquery that reads a distributed table, which the planner
	# keeps a SubPlan -- where the rows do not fit hash_mem, each outer row
	# scans them all again -- is an anti-join with its NULLs conditions beside
	# it (gp_subselect.c), as Cloudberry's planner makes a join of it.  It
	# answers as the SubPlan does, which (...) IS TRUE keeps one: over a
	# subquery with a NULL, over none, for an outer NULL, by <> ALL, of text,
	# over a join read once and a NOT IN nested in another; where neither side
	# can be NULL it is the anti-join alone, and a two-column NOT IN stays a
	# SubPlan.
	q 0 "CREATE TABLE nia (a int, t text) DISTRIBUTED BY (a);
	     CREATE TABLE nib (a int, t text) DISTRIBUTED BY (a);
	     CREATE TABLE nin (a int, t text) DISTRIBUTED BY (a);
	     CREATE TABLE nie (a int, t text) DISTRIBUTED BY (a);
	     CREATE TABLE nik (a int NOT NULL) DISTRIBUTED BY (a);
	     INSERT INTO nia SELECT i, 'v' || (i % 13) FROM generate_series(1, 300) i;
	     INSERT INTO nia VALUES (NULL, NULL);
	     INSERT INTO nib SELECT i * 2, 'v' || (i % 11) FROM generate_series(1, 100) i;
	     INSERT INTO nin SELECT * FROM nib; INSERT INTO nin VALUES (NULL, NULL);
	     INSERT INTO nik SELECT i * 3 FROM generate_series(1, 100) i;
	     ANALYZE nia; ANALYZE nib; ANALYZE nin; ANALYZE nie; ANALYZE nik;" >/dev/null
	same=0; n=0
	for c in "a NOT IN (SELECT a FROM nib)" "a NOT IN (SELECT a FROM nin)" \
		"a NOT IN (SELECT a FROM nie)" "a <> ALL (SELECT a FROM nib WHERE a > 50)" \
		"t NOT IN (SELECT t FROM nib)" "a NOT IN (SELECT nib.a FROM nib JOIN nin USING (t))" \
		"a NOT IN (SELECT a FROM nib WHERE a NOT IN (SELECT a FROM nin WHERE a > 100))"; do
		n=$((n + 1))
		got=$(q 0 "SET gp.optimizer = off; SELECT count(*), sum(a) FROM nia WHERE $c;")
		want=$(q 0 "SET gp.optimizer = off; SELECT count(*), sum(a) FROM nia WHERE ($c) IS TRUE;")
		[ -n "$got" ] && [ "$got" = "$want" ] && same=$((same + 1))
	done
	plan=$(q 0 "SET gp.optimizer = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM nia WHERE a NOT IN (SELECT a FROM nib);" | tr '\n' ' ')
	alone=$(q 0 "SET gp.optimizer = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM nik WHERE a NOT IN (SELECT a FROM nik WHERE a > 50);" | tr '\n' ' ')
	kept=$(q 0 "SET gp.optimizer = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM nia WHERE (a, t) NOT IN (SELECT a, t FROM nib);" | grep -c SubPlan)
	case "$plan / $alone" in
		*"InitPlan"*"Anti Join"*" / "*"Anti Join"*)
			[ "$same" = "$n" ] && [ "$kept" -ge 1 ] && [ "${alone#*InitPlan}" = "$alone" ] \
				&& ok "under the planner NOT IN of a distributed table is an anti-join, its NULLs conditions beside it, answering as the SubPlan does -- $n of them; of two columns it stays a SubPlan" \
				|| notok "NOT IN as an anti-join" "$same of $n / $kept / $alone" ;;
		*) notok "NOT IN as an anti-join" "$plan / $alone" ;;
	esac

	# With hash joins off -- a test turns them off to have its nested loop --
	# the loop's inner gather is keyed by the join's equality (gp_scan.c):
	# gathered once, its rows kept by their key, and each outer row given
	# those of its value.  It answers as the hash join does: a left join and
	# an inner one, NULL keys on both sides, an int against an int8, text,
	# two keys, a condition beside the key, NOT EXISTS, and a keyed gather
	# in a subquery run for each row.  With hash joins on the plan is the
	# hash join, as it was.
	q 0 "CREATE TABLE kga (i int, k int, t text) DISTRIBUTED BY (i);
	     CREATE TABLE kgb (i int, k int, k8 int8, t text) DISTRIBUTED BY (i);
	     INSERT INTO kga SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 997 END,
	                            CASE WHEN i % 19 = 0 THEN NULL ELSE 'v' || (i % 331) END
	                       FROM generate_series(1, 6000) i;
	     INSERT INTO kgb SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 7) % 1500 END,
	                            (i * 7) % 1500, 'v' || (i % 500)
	                       FROM generate_series(1, 9000) i;
	     ANALYZE kga; ANALYZE kgb;" >/dev/null
	same=0; n=0; keyed=0
	for c in "kga a LEFT JOIN kgb b ON a.k = b.k" "kga a JOIN kgb b ON a.k = b.k" \
		"kga a LEFT JOIN kgb b ON a.k = b.k8" "kga a LEFT JOIN kgb b ON a.t = b.t" \
		"kga a LEFT JOIN kgb b ON a.k = b.k AND a.t = b.t" \
		"kga a LEFT JOIN kgb b ON a.k + 1 = b.k AND b.i > a.i"; do
		n=$((n + 1))
		got=$(q 0 "SET gp.optimizer = off; SET enable_hashjoin = off; SET enable_mergejoin = off; SELECT count(*), sum(a.i), sum(b.i) FROM $c;")
		want=$(q 0 "SET gp.optimizer = off; SELECT count(*), sum(a.i), sum(b.i) FROM $c;")
		[ -n "$got" ] && [ "$got" = "$want" ] && same=$((same + 1))
		q 0 "SET gp.optimizer = off; SET enable_hashjoin = off; SET enable_mergejoin = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM $c;" \
			| grep -q "Lookup Key" && keyed=$((keyed + 1))
	done
	for c in "count(*) FROM kga a WHERE NOT EXISTS (SELECT 1 FROM kgb b WHERE b.k = a.k)" \
		"count(*) FROM kga a WHERE a.i < 200 AND (SELECT count(*) FROM kgb b JOIN kga c ON b.k = c.k WHERE b.i = a.i) > 0"; do
		n=$((n + 1))
		got=$(q 0 "SET gp.optimizer = off; SET enable_hashjoin = off; SET enable_mergejoin = off; SELECT $c;")
		want=$(q 0 "SET gp.optimizer = off; SELECT $c;")
		[ -n "$got" ] && [ "$got" = "$want" ] && same=$((same + 1))
	done
	hashed=$(q 0 "SET gp.optimizer = off; EXPLAIN (COSTS OFF) SELECT count(*) FROM kga a LEFT JOIN kgb b ON a.k = b.k;" | tr '\n' ' ')
	[ "$same" = "$n" ] && [ "$keyed" = 6 ] && [ "${hashed#*Hash Cond}" != "$hashed" ] &&
		[ "${hashed#*Lookup Key}" = "$hashed" ] \
		&& ok "with hash joins off a nested loop's inner gather is keyed by the join's equality, answering as the hash join does -- $n of them; with them on the plan is the hash join" \
		|| notok "keyed gathers" "$same of $n / $keyed keyed / $hashed"

	# No secret on the coordinator: ORCA is told, and the planner gathers.
	# None on the segments either -- a segment that has one takes the
	# coordinator's word only with it, and a transaction's two-phase commit
	# too (gp_dtx.c).
	for n in 1 2 0; do
		start_node "$n" "shared_preload_libraries = '$PRELOAD,gp_orca'"
	done
	out=$(printf '%s\n' "SET gp.optimizer_trace_fallback = on;" \
		"SELECT count(*), sum(a) FROM o WHERE b = 3;" | qf 0)
	case "$out" in
		*"a Motion, without gp.cluster_secret"*"100|49800") ok "without a secret nothing is dispatched as a plan, and the answer is the same" ;;
		*) notok "ORCA without a secret" "$out" ;;
	esac
	# ... nor is the coordinator's start taken from a gather: now() stays
	# here, and a cast to money goes still.
	out=$(q 0 "SET gp.optimizer = off; EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM stc WHERE ts > now() - interval '9 days' AND amt::money > '10'::money;" | grep -E "Remote SQL|Filter" | tr '\n' ' ')
	case "$out" in
		*"Filter:"*"now()"*"Remote SQL:"*"::money"*) ok "without a secret now() stays here, and a cast to money goes" ;;
		*) notok "a scan's stable conditions without a secret" "$out" ;;
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
		"DELETE FROM ds WHERE key = 1;" | qf 0 | grep -o 'INFO:  (slice.*' | tr '\n' '/')
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
		"DELETE FROM ds WHERE key = 7;" | qf 0 | grep -o 'INFO:  (slice.*' | tr '\n' '/')
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
		"SET gp.motion_cost_per_row = 0.5;" "SET gp.test_print_prefetch_joinqual = on;" \
		"SHOW gp.statement_mem;" | qf 0)
	[ "$out" = "2MB" ] && ok "Cloudberry's other settings are accepted, and say what they do here" \
		|| notok "Cloudberry's accepted settings" "$out"

	# A SET of the client's, outside a transaction block, reaches the
	# segments as it runs, as Cloudberry dispatches one: a value a segment
	# refuses fails the SET, which leaves the session as it was.  Here
	# temp_buffers, which a segment that has used a temporary table refuses
	# to change, and the coordinator, whose copy of the table is empty and
	# unused, does not.
	out=$(printf '%s\n' "SET client_min_messages = warning;" "SET gp.optimizer = off;" \
		"CREATE TEMP TABLE tbuf (a int) DISTRIBUTED BY (a);" "INSERT INTO tbuf SELECT generate_series(1, 10);" \
		"SET temp_buffers = 2000;" "SHOW temp_buffers;" "SELECT count(*) FROM tbuf;" | qf 0)
	case "$out" in
		*'invalid value for parameter "temp_buffers": 2000'*"8MB"*"10") ok "a SET a segment refuses fails as it runs, and the session goes on" ;;
		*) notok "a SET a segment refuses" "$out" ;;
	esac

	# SERIALIZABLE is REPEATABLE READ on a cluster, as in Cloudberry: no
	# node's serializable snapshot isolation sees another's rows.
	out=$(printf '%s\n' "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE;" \
		"SHOW default_transaction_isolation;" "RESET default_transaction_isolation;" \
		"BEGIN ISOLATION LEVEL SERIALIZABLE;" "SHOW transaction_isolation;" \
		"SELECT DISTINCT result FROM gp.exec_on_segments('SHOW transaction_isolation');" "COMMIT;" | qf 0)
	[ "$out" = "repeatable read
repeatable read
repeatable read" ] && ok "SERIALIZABLE, asked for, is REPEATABLE READ on the coordinator and the segments" \
		|| notok "SERIALIZABLE on a cluster" "$out"
	out=$(q 0 "ALTER ROLE CURRENT_USER SET default_transaction_isolation = 'serializable';")
	out2=$(q 0 "SHOW default_transaction_isolation;")
	q 0 "ALTER ROLE CURRENT_USER RESET default_transaction_isolation;" >/dev/null
	[ -z "$out" ] && [ "$out2" = "repeatable read" ] && ok "and so is a role's default of it" \
		|| notok "a role's default of SERIALIZABLE" "$out / $out2"

	# Cloudberry's EXPLAIN options: the slice table of the planner's route --
	# slice 0 the coordinator's, a Reader a gather, each gather labelled with
	# its slice -- a write's Primary Writer, the table in JSON; and where each
	# node's rows are, Entry above a gather and General for a VALUES list,
	# with gp.optimizer off, as Cloudberry prints them.
	q 0 "CREATE TABLE xe (a int, b int) DISTRIBUTED BY (a); CREATE TABLE xe2 (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	out=$(q 0 "SET enable_hashjoin = off; SET enable_nestloop = off;
			   EXPLAIN (SLICETABLE, COSTS OFF) SELECT * FROM xe JOIN xe2 USING (a);" | tr '\n' '|')
	case "$out" in
		*"on xe  (slice1; segments: 2)"*"on xe2  (slice2; segments: 2)"*"Slice 0: Dispatcher; root 0; parent -1; gang size 0|Slice 1: Reader; root 0; parent 0; gang size 2|Slice 2: Reader; root 0; parent 0; gang size 2"*)
			ok "EXPLAIN (SLICETABLE): the coordinator's slice and a gather's each, which its label names" ;;
		*) notok "EXPLAIN (SLICETABLE) under the planner" "$out" ;;
	esac
	out=$(q 0 "EXPLAIN (SLICETABLE, COSTS OFF) UPDATE xe SET b = 1;
			   EXPLAIN (SLICETABLE, COSTS OFF, FORMAT JSON) SELECT * FROM xe;" | tr -d ' \n')
	case "$out" in
		*"Slice0:PrimaryWriter;root0;parent-1;gangsize2"*'"SliceTable":[{"SliceID":0,"GangType":"Dispatcher"'*'"GangType":"Reader","Root":0,"Parent":0,"GangSize":2}]'*)
			ok "and a write's slice 0 a Primary Writer, and the table in JSON" ;;
		*) notok "EXPLAIN (SLICETABLE) of a write, and in JSON" "$out" ;;
	esac
	out=$(q 0 "SET gp.optimizer = off;
			   EXPLAIN (LOCUS, COSTS OFF) SELECT * FROM xe JOIN (VALUES (1), (2)) v(x) ON v.x = xe.b;" | tr '\n' '|')
	case "$out" in
		*"Locus: Entry"*"Gather Motion"*"Locus: Entry"*"Locus: General"*)
			ok "EXPLAIN (LOCUS): Entry above a gather, General for a VALUES list" ;;
		*) notok "EXPLAIN (LOCUS)" "$out" ;;
	esac
	out=$(q 0 "SET gp.enable_explain_allstat = on; SET gp.enable_offload_entry_to_qe = on; SELECT 1;")
	[ "$out" = "1" ] && ok "gp.enable_explain_allstat and gp.enable_offload_entry_to_qe are Cloudberry's settings" \
		|| notok "gp.enable_explain_allstat and gp.enable_offload_entry_to_qe" "$out"

	# EXPLAIN ANALYZE of the planner's route: what the statements a write
	# sends the segments wrote to the WAL -- an INSERT's COPY, an UPDATE and
	# a DELETE sent as they stand, a key's UPDATE moved by a Split -- each
	# gather's segments' memory, and their runs with
	# gp.enable_explain_allstat, a gather a LIMIT left open among them; and
	# the setting that asked the segments given back after the statement.
	q 0 "CREATE TABLE xw (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	out=""
	for stmt in "INSERT INTO xw SELECT g, g FROM generate_series(1, 100) g" \
			"UPDATE xw SET b = b + 1" "UPDATE xw SET a = a + 1000 WHERE a = 5" \
			"DELETE FROM xw WHERE a > 50"; do
		out="$out$(q 0 "SET gp.optimizer = off;
			EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $stmt;" | head -2 | tr '\n' '|')"
	done
	case "$out" in
		*"(Redistribute Motion) (actual rows=0.00 loops=1)|  WAL: records="*"(Dispatch) (actual rows=0.00 loops=1)|  WAL: records="*"(Explicit Redistribute Motion) (actual rows=0.00 loops=1)|  WAL: records="*"(Dispatch) (actual rows=0.00 loops=1)|  WAL: records="*)
			ok "EXPLAIN (ANALYZE, WAL) of the planner's writes: what the segments' statements wrote" ;;
		*) notok "EXPLAIN (ANALYZE, WAL) of the planner's writes" "$out" ;;
	esac
	out=$(q 0 "SET gp.optimizer = off; SET gp.enable_explain_allstat = on;
			   EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT * FROM xw;
			   EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT * FROM xw LIMIT 1;
			   SHOW gp.explain_instrument;" | tr '\n' '|')
	case "$out" in
		*"on xw  (slice1; segments: 2) (actual rows=49.00 loops=1)|  Segments: 2|  allstat: seg_firststart_total_ntuples/seg0_"*"//end|"*"(slice1)    Executor memory: "*" bytes avg x 2 workers"*"Limit (actual rows=1.00 loops=1)"*"allstat: seg_firststart_total_ntuples/seg0_"*"/seg1_"*"//end"*"|0|")
			ok "a gather's segments' runs and memory, a LIMIT's open gather's too, and the setting given back" ;;
		*) notok "EXPLAIN ANALYZE of a gather" "$out" ;;
	esac

	###########################################################################
	echo "12. DISTRIBUTED BY as Cloudberry checks it, and what the segments say"
	###########################################################################
	# distribution.c, gp_policy.c and gp_label.c: a key checked against the
	# table it is for, in Cloudberry's words; a column's operator class, which
	# the table depends on; and the rules for the rest of a cluster's DDL.
	out=$(q 0 "CREATE TABLE xk (a int, b int) DISTRIBUTED BY (b, B);")
	out2=$(q 0 "CREATE TABLE xk (a int, b int) DISTRIBUTED BY (a, c);")
	case "$out|$out2" in
		*"duplicate column in DISTRIBUTED BY clause"*"LINE 1:"*'column "c" named in DISTRIBUTED BY clause does not exist'*"LINE 1:"*)
			ok "a key column named twice, or not the table's, is refused where it is written" ;;
		*) notok "DISTRIBUTED BY's columns" "$out / $out2" ;;
	esac
	out=$(q 0 "CREATE TABLE xk (a int PRIMARY KEY, b int) DISTRIBUTED RANDOMLY;")
	out2=$(q 0 "CREATE TABLE xk (a int UNIQUE, b int) DISTRIBUTED BY (b);")
	out3=$(q 0 "CREATE TABLE xk (p point) DISTRIBUTED BY (p);")
	case "$out|$out2|$out3" in
		*"PRIMARY KEY and DISTRIBUTED RANDOMLY are incompatible"*"|"*"UNIQUE constraint and DISTRIBUTED BY definitions are incompatible"*"|"*'data type point has no default operator class for access method "hash"'*)
			ok "and so is a key no unique constraint takes in, or of a type that cannot be hashed" ;;
		*) notok "DISTRIBUTED BY and constraints" "$out / $out2 / $out3" ;;
	esac
	q 0 "CREATE TABLE xk (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	out=$(q 0 "CREATE UNIQUE INDEX xk_b ON xk (b);")
	out2=$(q 0 "ALTER TABLE xk SET DISTRIBUTED BY (a);")
	out3=$(q 0 "ALTER TABLE ONLY pg_class SET DISTRIBUTED RANDOMLY;")
	case "$out|$out2|$out3" in
		*"UNIQUE index must contain all columns in the table's distribution key"*'already set to (a)'*'"pg_class" is a system catalog'*)
			ok "a unique index must take the key in; a policy already set is left, with Cloudberry's WARNING; a catalog has none" ;;
		*) notok "unique indexes and SET DISTRIBUTED" "$out / $out2 / $out3" ;;
	esac

	# SET DISTRIBUTED after ALTER PARTITION: the leaf's ALTER TABLE, as
	# Cloudberry allows of a leaf; any other command of a leaf is refused.
	q 0 "CREATE TABLE xpd (i int, k int) DISTRIBUTED BY (i) PARTITION BY RANGE (i) (START (1) END (10) EVERY (1));" >/dev/null
	out=$(q 0 "ALTER TABLE xpd ALTER PARTITION FOR (5) SET DISTRIBUTED BY (i);")
	out2=$(q 0 "ALTER TABLE xpd ALTER PARTITION FOR (5) ADD COLUMN z int;")
	case "$out|$out2" in
		*'WARNING:  distribution policy of relation "xpd_1_prt_5" already set to (i)'*'HINT:  Use ALTER TABLE "xpd_1_prt_5" SET WITH (REORGANIZE=TRUE) DISTRIBUTED BY (i) to force redistribution'*"|"*'table "xpd_1_prt_5" is not partitioned'*)
			ok "ALTER PARTITION FOR (5) SET DISTRIBUTED BY is the leaf's, with Cloudberry's WARNING" ;;
		*) notok "SET DISTRIBUTED BY after ALTER PARTITION" "$out / $out2" ;;
	esac

	# A column's operator class: its hash decides the segment, and the table
	# depends on it, on every node.
	cat > "$ROOT/absops.sql" <<'SQL'
CREATE FUNCTION abseq(int, int) RETURNS bool AS $$ SELECT abs($1) = abs($2) $$
	LANGUAGE sql STRICT IMMUTABLE;
CREATE OPERATOR |=| (PROCEDURE = abseq, LEFTARG = int, RIGHTARG = int,
	COMMUTATOR = |=|, HASHES);
CREATE FUNCTION abshash(int) RETURNS int AS $$ SELECT abs($1) $$
	LANGUAGE sql STRICT IMMUTABLE;
CREATE OPERATOR CLASS abs_ops FOR TYPE int4 USING hash AS
	OPERATOR 1 |=|, FUNCTION 1 abshash(int);
CREATE TABLE xo (a int) DISTRIBUTED BY (a abs_ops);
INSERT INTO xo SELECT g FROM generate_series(-20, 20) g;
SQL
	qf 0 < "$ROOT/absops.sql" >/dev/null
	out=$(q 0 "SELECT gp_sql.distribution('xo'::regclass);")
	out2=$(q 0 "SELECT count(*) FROM (SELECT abs(a) FROM gp.dist_random(NULL::xo) GROUP BY 1 HAVING count(DISTINCT gp_segment_id) > 1) s;")
	out3=$(q 0 "DROP OPERATOR CLASS abs_ops USING hash;")
	[ "$out|$out2" = "(a public.abs_ops)|0" ] && case "$out3" in
		*"cannot drop operator class abs_ops"*"table xo depends on operator class abs_ops"*) true ;;
		*) false ;;
	esac && ok "DISTRIBUTED BY (a abs_ops): its hash places the rows, and the table depends on the class" \
		|| notok "an operator class in DISTRIBUTED BY" "$out / $out2 / $out3"
	q 0 "DROP OPERATOR CLASS abs_ops USING hash CASCADE;" >/dev/null
	out=$(q 0 "SELECT count(*) FROM pg_class WHERE relname = 'xo';")
	out2=$(q 1 "SELECT count(*) FROM pg_class WHERE relname = 'xo';")
	[ "$out|$out2" = "0|0" ] && ok "and CASCADE drops the table, on the segments too" \
		|| notok "DROP OPERATOR CLASS ... CASCADE" "$out / $out2"

	# bit and bit varying hash, as Cloudberry's catalog hashes them; a
	# constant of another type of the key's hash family finds its segment.
	out=$(printf '%s\n' "CREATE TABLE xbit (x varbit) DISTRIBUTED BY (x);" \
		"INSERT INTO xbit VALUES ('0101010');" \
		"CREATE TABLE xi2 (id int2) DISTRIBUTED BY (id);" \
		"INSERT INTO xi2 VALUES (1);" \
		"SET gp.test_print_direct_dispatch_info = on;" \
		"SELECT * FROM xbit WHERE x = '0101010';" \
		"SELECT * FROM xi2 WHERE id = 1::int8;" | qf 0 | tr '\n' ' ')
	case "$out" in
		*"SINGLE content 0101010 "*"SINGLE content 1 ") ok "a bit string is a key, and 1::int8 finds an int2 key's segment" ;;
		*) notok "bit keys and cross-type direct dispatch" "$out" ;;
	esac

	# Cloudberry's legacy hash, a cdbhash_*_ops class: FNV-1, reduced on two
	# segments by a bitmask -- the odd keys on the first, the even on the
	# second -- and found there directly; and the class gp.use_legacy_hashops
	# gives a key that names none, which the label then names.  (ORCA's
	# Motions hash by it in section 10.)
	out=$(printf '%s\n' "CREATE TABLE xlg (a int) DISTRIBUTED BY (a cdbhash_int4_ops);" \
		"INSERT INTO xlg SELECT generate_series(1, 6);" \
		"SELECT string_agg(a::text, ',' ORDER BY a) FROM xlg GROUP BY gp_segment_id ORDER BY gp_segment_id;" \
		"SET gp.test_print_direct_dispatch_info = on;" \
		"SELECT * FROM xlg WHERE a = 4;" \
		"RESET gp.test_print_direct_dispatch_info;" \
		"SET gp.use_legacy_hashops = on;" \
		"CREATE TABLE xlg2 (k int, a int) DISTRIBUTED BY (k);" \
		"SELECT gp_sql.distribution('xlg2'::regclass);" | qf 0 | tr '\n' ' ')
	case "$out" in
		"1,3,5 2,4,6 "*"SINGLE content 4 (k pg_catalog.cdbhash_int4_ops) ") ok "a cdbhash_*_ops key hashes as Cloudberry's legacy cdbhash, and gp.use_legacy_hashops gives one" ;;
		*) notok "the legacy hash" "$out" ;;
	esac

	# CREATE TABLE AS takes the key of its query's rows where they become
	# its columns, and LIKE the table's it is made like.
	out=$(printf '%s\n' "CREATE TABLE xq AS SELECT 1 AS c, a FROM (SELECT a FROM d) s;" \
		"SELECT gp_sql.distribution('xq'::regclass);" \
		"CREATE TABLE xl (LIKE ds);" "SELECT gp_sql.distribution('xl'::regclass);" | qf 0 | grep -v NOTICE | grep -v HINT | tr '\n' ' ')
	[ "$out" = "(a) (key) " ] && ok "CREATE TABLE AS keeps its query's key, and LIKE its table's" \
		|| notok "CTAS and LIKE distributions" "$out"

	# What a segment says -- a trigger's NOTICE -- is the client's, once;
	# what it says of a DDL statement it was sent is the coordinator's.
	cat > "$ROOT/notice.sql" <<'SQL'
CREATE TABLE xn (a int, b int) DISTRIBUTED BY (a);
CREATE FUNCTION xn_f() RETURNS trigger LANGUAGE plpgsql AS
	$$ BEGIN RAISE NOTICE 'row % on a segment', NEW.a; RETURN NEW; END $$;
CREATE TRIGGER xn_t AFTER INSERT ON xn FOR EACH ROW EXECUTE FUNCTION xn_f();
INSERT INTO xn VALUES (1, 1);
CREATE TABLE xn2 (a int, b int) INHERITS (xn);
SQL
	out=$(qf 0 < "$ROOT/notice.sql" | grep -c 'NOTICE:  row 1 on a segment')
	out2=$(qf 0 < /dev/null; q 0 "DROP TABLE xn2; CREATE TABLE xn2 (a int, b int) INHERITS (xn);" | grep -c 'merging')
	[ "$out|$out2" = "1|2" ] && ok "a trigger's NOTICE on a segment reaches the client once; DDL's are the coordinator's" \
		|| notok "notices from the segments" "$out / $out2"
	out=$(q 0 "CREATE TRIGGER xn_s AFTER INSERT ON xn FOR EACH STATEMENT EXECUTE FUNCTION xn_f();")
	out2=$(printf '%s\n' "SET gp.enable_statement_trigger = on;" \
		"CREATE TRIGGER xn_s AFTER INSERT ON xn FOR EACH STATEMENT EXECUTE FUNCTION xn_f();" | qf 0)
	case "$out|$out2" in
		*"Triggers for statements are not yet supported"*"|") ok "a statement trigger only with gp.enable_statement_trigger, as in Cloudberry" ;;
		*) notok "statement triggers" "$out / $out2" ;;
	esac
	q 0 "CREATE TABLE xu (a int, b int) DISTRIBUTED BY (a); CREATE FUNCTION xu_f() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN RETURN NEW; END \$\$; CREATE TRIGGER xu_t BEFORE UPDATE ON xu FOR EACH ROW EXECUTE FUNCTION xu_f(); INSERT INTO xu VALUES (1, 1);" >/dev/null
	out=$(q 0 "UPDATE xu SET a = a + 1;")
	case "$out" in
		*"UPDATE on distributed key column not allowed on relation with update triggers"*)
			ok "the key of a table with UPDATE triggers is not changed, in Cloudberry's words" ;;
		*) notok "a key change under UPDATE triggers" "$out" ;;
	esac

	# A replicated table's row, changed by a statement that reads another
	# table, is changed on every segment and counted once; its system columns
	# are none of the user's, and gp_segment_id is another relation's.
	out=$(printf '%s\n' "CREATE TABLE xr (x int, y int) DISTRIBUTED REPLICATED;" \
		"CREATE TABLE xrd (a int) DISTRIBUTED BY (a);" \
		"INSERT INTO xr VALUES (1, 1), (2, 1), (2, 1);" \
		"INSERT INTO xrd VALUES (1), (2);" \
		"UPDATE xr SET y = 3 FROM xrd WHERE xrd.a = xr.x;" \
		"DELETE FROM xr USING xrd WHERE xrd.a = xr.x AND xr.x = 2;" | qf 0 | tr '\n' ' ')
	out2=$(q 0 "SELECT gp_segment_id, x, y FROM gp.dist_random(NULL::xr) ORDER BY 1;" | tr '\n' ' ')
	[ "$out|$out2" = "|0|1|3 1|1|3 " ] && ok "UPDATE and DELETE of a replicated table that read another: every copy, counted once" \
		|| notok "writes to a replicated table" "$out / $out2"
	out=$(q 0 "SELECT ctid FROM xr;")
	out2=$(q 0 "SELECT count(*) FROM xrd, xr WHERE xr.x = xrd.a AND gp_segment_id = expected_seg(xrd.a, 2);")
	case "$out|$out2" in
		*'column "ctid" does not exist'*"|1") ok "a replicated table has no system column here, and gp_segment_id is the other table's" ;;
		*) notok "a replicated table's system columns" "$out / $out2" ;;
	esac

	# Cloudberry reserves gp_ for schemas, and keeps pg_toast where it is.
	out=$(q 0 "CREATE SCHEMA gp_mine;")
	out2=$(q 0 "ALTER SCHEMA pg_toast RENAME TO toast;")
	case "$out|$out2" in
		*'unacceptable schema name "gp_mine"'*'permission denied to ALTER SCHEMA "pg_toast"'*)
			ok "gp_ is reserved for system schemas, and pg_toast is not renamed" ;;
		*) notok "reserved schema names" "$out / $out2" ;;
	esac

	###########################################################################
	echo "13. distributed transactions: two-phase commit and distributed snapshots"
	###########################################################################
	# gp_dtx.c: a transaction that wrote on a segment is prepared there under
	# the coordinator's transaction ID, whose commit record decides it, and
	# each statement is sent the coordinator's snapshot of it, which a
	# segment's snapshots are made to agree with.  Cloudberry's fault
	# injector (gp_fault.c) holds a transaction between its phases.
	out=$(q 0 "CREATE EXTENSION gp_inject_fault;")
	out2=$(q 0 "CREATE TABLE dtx (a int, b int) DISTRIBUTED BY (a); INSERT INTO dtx SELECT i, i FROM generate_series(1, 20) i;")
	[ -z "$out$out2" ] && ok "gp_inject_fault, Cloudberry's fault injector, is created" \
		|| notok "CREATE EXTENSION gp_inject_fault" "$out / $out2"

	# Cloudberry's fault of a segment's SET, set_variable_fault, is met as a
	# SET runs, which it fails: the coordinator's value goes back, and the
	# segments are told it with the next statement.  A SET in a transaction
	# block or a DO waits for the next statement sent, as before, and the
	# fault is met there.
	out=$(printf '%s\n' "SET datestyle = 'German';" "SELECT count(*) FROM dtx;" \
		"SELECT gp_inject_fault('set_variable_fault', 'error', $(dbid 1));" \
		"SET datestyle = 'SQL, MDY';" "SHOW datestyle;" \
		"SELECT DISTINCT result FROM gp.exec_on_segments('SHOW datestyle');" \
		"SELECT gp_inject_fault('set_variable_fault', 'reset', $(dbid 1));" | qf 0 | tr '\n' '|')
	case "$out" in
		"20|Success:|"*"ERROR:  fault triggered, fault name:'set_variable_fault' fault type:'error'"*"|German, DMY|German, DMY|Success:|")
			ok "set_variable_fault fails the SET as it runs, and the segments keep the value the coordinator has" ;;
		*) notok "set_variable_fault at a SET" "$out" ;;
	esac
	out=$(printf '%s\n' "SELECT gp_inject_fault('set_variable_fault', 'error', $(dbid 1));" \
		"BEGIN;" "SET datestyle = 'SQL, MDY';" "SELECT 'set';" "SELECT count(*) FROM dtx;" "ROLLBACK;" \
		"SELECT gp_inject_fault('set_variable_fault', 'reset', $(dbid 1));" \
		"SELECT gp_inject_fault('set_variable_fault', 'error', $(dbid 1));" \
		"DO \$\$ BEGIN SET datestyle = 'Postgres, MDY'; END \$\$;" "SELECT 'done';" "SELECT count(*) FROM dtx;" \
		"SELECT gp_inject_fault('set_variable_fault', 'reset', $(dbid 1));" | qf 0 | tr '\n' '|')
	case "$out" in
		"Success:|set|"*"ERROR:  fault triggered, fault name:'set_variable_fault'"*"|Success:|Success:|done|"*"ERROR:  fault triggered, fault name:'set_variable_fault'"*"|Success:|")
			ok "and a SET in a transaction block, or in a DO, meets it with the next statement sent" ;;
		*) notok "set_variable_fault after a SET in a block or a DO" "$out" ;;
	esac

	out=$(printf '%s\n' "SELECT gp_inject_fault_infinite('dtm_broadcast_prepare', 'skip', 1);" \
		"SELECT count(*) FROM dtx;" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'status', 1);" \
		"INSERT INTO dtx VALUES (21, 21);" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'status', 1);" \
		"UPDATE dtx SET b = b;" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'status', 1);" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'reset', 1);" | qf 0 |
		grep -o "num times hit:'[0-9]*'" | tr '\n' ' ')
	[ "$out" = "num times hit:'0' num times hit:'0' num times hit:'1' " ] \
		&& ok "a transaction that read, or wrote on one segment alone, commits in one phase, one that wrote on both in two" \
		|| notok "which transactions are prepared" "$out"
	# The coordinator asks nobody which wrote: each segment says so with every
	# answer, as its part's transaction ID (gp_dtx.c) -- a write the
	# coordinator did not send as one, a function a query runs on the
	# segments, included: those parts are prepared, not committed as a
	# reader's would be.
	q 0 "CREATE TABLE dtxw (a int) DISTRIBUTED RANDOMLY;" >/dev/null
	q 0 "CREATE FUNCTION dtx_write() RETURNS int AS 'INSERT INTO dtxw VALUES (1) RETURNING 1' LANGUAGE sql VOLATILE;" >/dev/null
	out=$(printf '%s\n' "SELECT gp_inject_fault_infinite('dtm_broadcast_prepare', 'skip', 1);" \
		"SELECT count(*) FROM gp.exec_on_segments('SELECT dtx_write()');" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'status', 1);" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'reset', 1);" | qf 0 |
		grep -o "num times hit:'[0-9]*'")
	out2=$(q 0 "SELECT count(*) FROM dtxw;")
	q 0 "DROP FUNCTION dtx_write(); DROP TABLE dtxw;" >/dev/null
	[ "$out|$out2" = "num times hit:'1'|2" ] \
		&& ok "a segment says with its answer that it wrote, even where a function it ran wrote, and its part is prepared" \
		|| notok "the transaction ID a segment reports" "$out / $out2"

	# A commit held between its phases: decided by its commit record, and in
	# progress for every other session until its second phase is done, as
	# Cloudberry's is -- the coordinator sends COMMIT PREPARED before its
	# transaction ends for the others (O33).
	q 0 "SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'suspend', 1);" >/dev/null
	q 0 "INSERT INTO dtx SELECT i, i FROM generate_series(22, 60) i;" >/dev/null 2>&1 &
	writer=$!
	q 0 "SELECT gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 1, 1);" >/dev/null
	gid=$(q 1 "SELECT gid FROM pg_prepared_xacts;")
	gid2=$(q 2 "SELECT gid FROM pg_prepared_xacts;")
	status=$(q 0 "SELECT pg_xact_status('${gid#gp_dtx_}'::xid8);")
	case "$gid|$gid2|$status" in
		"gp_dtx_"[0-9]*"|$gid|in progress")
			ok "its parts are prepared on both segments under the coordinator's transaction ID, in progress for the others until they are told" ;;
		*) notok "the prepared parts and their decision" "$gid / $gid2 / $status" ;;
	esac
	q 0 "SELECT count(*), sum(b) FROM dtx;" > "$ROOT/dtx_reader.out" 2>&1 &
	reader=$!
	sleep 1
	out=$(q 1 "SELECT wait_event FROM pg_stat_activity WHERE backend_type = 'client backend' AND wait_event_type = 'Lock';")
	q 0 "SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'resume', 1);" >/dev/null
	wait "$writer" "$reader"
	q 0 "SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'reset', 1);" >/dev/null
	out2=$(cat "$ROOT/dtx_reader.out")
	out3=$(q 0 "SELECT count(*), sum(b) FROM dtx;")
	p1=$(q 1 "SELECT count(*) FROM pg_prepared_xacts;")
	[ "$out|$out2|$out3|$p1" = "|21|231|60|1830|0" ] \
		&& ok "a statement meeting a transaction between its phases sees it in progress, waiting for nothing, and one after sees it" \
		|| notok "a statement meeting a transaction between its phases" "$out / $out2 / $out3 / $p1"

	# A segment that cannot prepare.
	q 0 "SELECT gp_inject_fault('start_prepare', 'error', $(dbid 2));" >/dev/null
	out=$(q 0 "INSERT INTO dtx SELECT i, i FROM generate_series(61, 70) i;")
	q 0 "SELECT gp_inject_fault('start_prepare', 'reset', $(dbid 2));" >/dev/null
	out2=$(q 0 "SELECT count(*) FROM dtx;")
	p1=$(q 1 "SELECT count(*) FROM pg_prepared_xacts;")
	p2=$(q 2 "SELECT count(*) FROM pg_prepared_xacts;")
	case "$out|$out2|$p1|$p2" in
		*"fault triggered, fault name:'start_prepare'"*"segment 1"*"|60|0|0")
			ok "a segment that fails to prepare fails the commit, and what the other prepared is rolled back" ;;
		*) notok "a failure in the first phase" "$out / $out2 / $p1 / $p2" ;;
	esac

	# A second phase a segment refuses is tried again over a connection of
	# its own, as Cloudberry's coordinator retries it over a new gang, saying
	# so (doNotifyingCommitPrepared(), cdbtm.c); the gang goes.
	q 0 "CREATE TABLE dtxf (a int, b int) DISTRIBUTED BY (a);" >/dev/null
	q 0 "SELECT gp_inject_fault('finish_prepared_start_of_function', 'error', $(dbid 1));" >/dev/null
	out=$(q 0 "INSERT INTO dtxf SELECT i, i FROM generate_series(1, 20) i;")
	q 0 "SELECT gp_inject_fault('finish_prepared_start_of_function', 'reset', $(dbid 1));" >/dev/null
	out2=$(q 0 "SELECT count(*), sum(b) FROM dtxf;")
	p1=$(q 1 "SELECT count(*) FROM pg_prepared_xacts;")
	case "$out|$out2|$p1" in
		*"'Commit Prepared' broadcast failed to one or more segments. Retrying ... try 1"*"Releasing segworker group to retry broadcast."*"|20|210|0")
			ok "a second phase a segment refuses is retried over a new connection, in Cloudberry's words" ;;
		*) notok "a second phase refused once" "$out / $out2 / $p1" ;;
	esac
	q 0 "TRUNCATE dtxf;" >/dev/null

	# One that goes on failing: the coordinator's transaction ends all the
	# same, its commit decided, and the recovery process is left the part; a
	# statement whose snapshot says it committed waits on that segment for
	# it meanwhile (gp_dtx.c), then sees it.
	q 0 "SELECT gp_inject_fault('dtx_recovery_round', 'suspend', 1);" >/dev/null
	q 0 "SELECT gp_inject_fault_infinite('finish_prepared_start_of_function', 'error', $(dbid 1));" >/dev/null
	out=$(q 0 "INSERT INTO dtxf SELECT i, i FROM generate_series(1, 20) i;")
	q 0 "SELECT count(*), sum(b) FROM dtxf;" > "$ROOT/dtxf_reader.out" 2>&1 &
	reader=$!
	sleep 1
	out2=$(q 1 "SELECT wait_event FROM pg_stat_activity WHERE backend_type = 'client backend' AND wait_event_type = 'Lock';")
	q 0 "SELECT gp_inject_fault('finish_prepared_start_of_function', 'reset', $(dbid 1));" >/dev/null
	q 0 "SELECT gp_inject_fault('dtx_recovery_round', 'reset', 1);" >/dev/null
	wait "$reader"
	out3=$(cat "$ROOT/dtxf_reader.out")
	p1=$(q 1 "SELECT count(*) FROM pg_prepared_xacts;")
	case "$out|$out2|$out3|$p1" in
		*"was committed, but 1 of its segments have not been told yet"*"|transactionid|20|210|0")
			ok "a part whose second phase failed is the recovery process's, and a statement that sees it committed waits for it there" ;;
		*) notok "a second phase that failed on a segment" "$out / $out2 / $out3 / $p1" ;;
	esac
	q 0 "DROP TABLE dtxf;" >/dev/null

	# debug_dtm_action's failures of a function's subtransactions, where
	# Cloudberry's raise them (gp_dtm_debug.c): a block's rollback segment 0
	# fails escapes the block's handler, and the handler around it, as that
	# segment goes on failing; and a begin it fails fails the block's entry.
	q 0 "CREATE TABLE dtxb (a int) DISTRIBUTED BY (a);
		CREATE FUNCTION dtxb_f() RETURNS text LANGUAGE plpgsql AS \$\$
		BEGIN
			INSERT INTO dtxb VALUES (1);
			BEGIN
				BEGIN
					PERFORM 1 / 0;
				EXCEPTION WHEN division_by_zero THEN
					RETURN 'inner handler';
				END;
			EXCEPTION WHEN OTHERS THEN
				RETURN 'outer handler';
			END;
		END \$\$;" >/dev/null
	dtm="SET gp.debug_dtm_action_segment = 0; SET gp.debug_dtm_action_target = protocol;"
	out=$(q 0 "$dtm SET gp.debug_dtm_action_protocol = subtransaction_rollback;
		SET gp.debug_dtm_action = fail_end_command; SELECT dtxb_f();")
	out2=$(q 0 "$dtm SET gp.debug_dtm_action_protocol = subtransaction_begin;
		SET gp.debug_dtm_action = fail_begin_command; SELECT dtxb_f();")
	out3=$(q 0 "SELECT dtxb_f(); SELECT count(*) FROM dtxb;")
	case "$out|$out2|$out3" in
		"ERROR:  Raise error for debug_dtm_action = 3, debug_dtm_action_protocol = Rollback Current Subtransaction"*"line 11 at RETURN"*"|ERROR:  Raise ERROR for debug_dtm_action = 2, debug_dtm_action_protocol = Begin Internal Subtransaction"*"line 4 during statement block entry"*"|inner handler"*"1")
			ok "a function's subtransaction a segment fails to roll back fails the handlers around it, and one it fails to begin fails the block's entry" ;;
		*) notok "debug_dtm_action's subtransaction failures" "$out / $out2 / $out3" ;;
	esac
	q 0 "DROP FUNCTION dtxb_f(); DROP TABLE dtxb;" >/dev/null

	# What gp.test_print_direct_dispatch_info says of the two phases, in
	# Cloudberry's words (doDispatchDtxProtocolCommand(), cdbtm.c): each
	# command, before it is sent, and the segments it goes to -- the parts
	# that wrote, for the two phases, and for a one-phase commit and a
	# rollback before any part is prepared, every segment the transaction
	# reached.  A part that wrote alone commits in one phase, as Cloudberry's
	# does; a transaction that only read says nothing; and a rollback is
	# named by how far the first phase got -- none of the parts that wrote
	# prepared, a fault once every part is prepared, where Cloudberry's is,
	# and a segment that fails to.
	out=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" \
		"CREATE TABLE dtxi (a int) DISTRIBUTED BY (a);" \
		"INSERT INTO dtxi VALUES (1);" \
		"BEGIN;" "INSERT INTO dtxi VALUES (100);" "ROLLBACK;" \
		"SELECT count(*) FROM dtxi;" \
		"INSERT INTO dtxi SELECT generate_series(2, 10);" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'error', 1);" \
		"INSERT INTO dtxi SELECT generate_series(11, 20);" \
		"SELECT gp_inject_fault('start_prepare', 'error', $(dbid 2));" \
		"INSERT INTO dtxi SELECT generate_series(11, 20);" \
		"RESET gp.test_print_direct_dispatch_info;" \
		"SELECT gp_inject_fault('dtm_broadcast_prepare', 'reset', 1);" \
		"SELECT gp_inject_fault('start_prepare', 'reset', $(dbid 2));" | qf 0)
	info=$(printf '%s\n' "$out" | grep -o 'INFO:  Distributed.*' | tr '\n' '/')
	out2=$(q 0 "SELECT count(*) FROM dtxi;")
	p1=$(q 1 "SELECT count(*) FROM pg_prepared_xacts;")
	p2=$(q 2 "SELECT count(*) FROM pg_prepared_xacts;")
	q 0 "DROP TABLE dtxi;" >/dev/null
	dtxc="INFO:  Distributed transaction command"
	expect="$dtxc 'Distributed Prepare' to ALL contents: 0 1/$dtxc 'Distributed Commit Prepared' to ALL contents: 0 1/"
	expect="$expect$dtxc 'Distributed Commit (one-phase)' to SINGLE content/"
	expect="$expect$dtxc 'Distributed Abort (No Prepared)' to SINGLE content/"
	expect="$expect$dtxc 'Distributed Prepare' to ALL contents: 0 1/$dtxc 'Distributed Commit Prepared' to ALL contents: 0 1/"
	expect="$expect$dtxc 'Distributed Prepare' to ALL contents: 0 1/$dtxc 'Distributed Abort Prepared' to ALL contents: 0 1/"
	expect="$expect$dtxc 'Distributed Prepare' to ALL contents: 0 1/$dtxc 'Distributed Abort (Some Prepared)' to ALL contents: 0 1/"
	case "$out|$info|$out2|$p1|$p2" in
		*"fault name:'dtm_broadcast_prepare'"*"fault name:'start_prepare'"*"|$expect|10|0|0")
			ok "gp.test_print_direct_dispatch_info names each command of the two phases, and the segments it goes to" ;;
		*) notok "the two phases' INFO lines" "$info / $out2 / $p1 / $p2" ;;
	esac

	# The segments a one-phase commit and a rollback name: every one the
	# transaction's dispatches reached, whether or not a part wrote there, in
	# the order they were first reached, as Cloudberry names its dtxSegments
	# (addToGxactDtxSegments(), cdbtm.c) -- a write's, and in a transaction
	# block a read's too.  A write that changed nothing commits in one phase
	# on both segments; a block that read, then rolled back, names what it
	# read; one whose first statement went to segment 1 alone names it first.
	q 0 "CREATE TABLE dtxr (a int, b int) DISTRIBUTED BY (a);
		 INSERT INTO dtxr SELECT i, i FROM generate_series(1, 10) i;" >/dev/null
	k1=$(q 0 "SELECT min(a) FROM dtxr WHERE gp_segment_id = 1;")
	out=$(printf '%s\n' "SET gp.test_print_direct_dispatch_info = on;" \
		"UPDATE dtxr SET b = 0 WHERE b < 0;" \
		"BEGIN;" "SELECT count(*) FROM dtxr;" "ROLLBACK;" \
		"BEGIN;" "SELECT b FROM dtxr WHERE a = $k1;" "SELECT count(*) FROM dtxr;" "ROLLBACK;" \
		"SELECT count(*) FROM dtxr;" \
		"RESET gp.test_print_direct_dispatch_info;" | qf 0)
	info=$(printf '%s\n' "$out" | grep -o 'INFO:  Distributed.*' | tr '\n' '/')
	q 0 "DROP TABLE dtxr;" >/dev/null
	expect="$dtxc 'Distributed Commit (one-phase)' to ALL contents: 0 1/"
	expect="$expect$dtxc 'Distributed Abort (No Prepared)' to ALL contents: 0 1/"
	expect="$expect$dtxc 'Distributed Abort (No Prepared)' to ALL contents: 1 0/"
	case "$k1|$info" in
		[0-9]*"|$expect")
			ok "a one-phase commit and a rollback name every segment the transaction reached, in the order it reached them" ;;
		*) notok "the segments a transaction reached, in its INFO lines" "$k1 / $info" ;;
	esac

	# The coordinator goes down between the phases.  Its postmaster restarts
	# it; the statement after waits on the segments until the recovery
	# process commits the parts by the commit record.
	q 0 "SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'panic', 1);" >/dev/null
	q 0 "INSERT INTO dtx SELECT i, i FROM generate_series(61, 70) i;" >/dev/null 2>&1
	for i in $(seq 1 60); do
		out=$(q 0 "SELECT count(*) FROM dtx;" 2>/dev/null)
		[ "$out" = "70" ] && break
		sleep 0.5
	done
	p1=$(q 1 "SELECT count(*) FROM pg_prepared_xacts;")
	p2=$(q 2 "SELECT count(*) FROM pg_prepared_xacts;")
	log=$(grep -c "distributed transaction recovery: COMMIT PREPARED" "$ROOT/node0.log")
	[ "$out|$p1|$p2" = "70|0|0" ] && [ "$log" -ge 1 ] \
		&& ok "a coordinator that went down between the phases: its recovery process commits the parts" \
		|| notok "recovery after the coordinator went down" "$out / $p1 / $p2 / $log"

	# gp.dtx_recovered(), which gpstart waits for as Cloudberry's pg_ctl
	# waits for "DTM recovered": false from the coordinator's start until a
	# round of its recovery process has reached every node.
	"$BINDIR/pg_ctl" -D "$(datadir 2)" -m fast stop >/dev/null 2>&1
	logsize=$(stat -c %s "$ROOT/node0.log")
	"$BINDIR/pg_ctl" -D "$(datadir 0)" -l "$ROOT/node0.log" -m fast -w -t 30 restart >/dev/null 2>&1
	sleep 2
	down=$(q 0 "SELECT gp.dtx_recovered();")
	"$BINDIR/pg_ctl" -D "$(datadir 2)" -l "$ROOT/node2.log" -w -t 30 start >/dev/null 2>&1
	for i in $(seq 1 40); do
		up=$(q 0 "SELECT gp.dtx_recovered();")
		[ "$up" = "t" ] && break
		sleep 0.5
	done
	log=$(tail -c +"$((logsize + 1))" "$ROOT/node0.log" | grep -c "DTM Started")
	[ "$down|$up|$log" = "f|t|1" ] \
		&& ok "gp.dtx_recovered(): not while a segment cannot be reached since the coordinator started, then \"DTM Started\"" \
		|| notok "gp.dtx_recovered()" "$down / $up / $log"

	# REPEATABLE READ: the snapshot taken on the coordinator before another
	# transaction committed hides it on every segment, though each segment's
	# own snapshot, taken after, would see it.
	printf '%s\n' "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT 'established';" \
		"SELECT pg_sleep(2);" "SELECT count(*), sum(b) FROM dtx;" "COMMIT;" |
		qf 0 > "$ROOT/dtx_rr.out" 2>&1 &
	rr=$!
	sleep 1
	q 0 "INSERT INTO dtx SELECT i, i FROM generate_series(71, 80) i;" >/dev/null
	wait "$rr"
	out=$(tail -1 "$ROOT/dtx_rr.out")
	out2=$(q 0 "SELECT count(*), sum(b) FROM dtx;")
	[ "$out|$out2" = "70|2485|80|3240" ] \
		&& ok "a snapshot taken before a commit hides it on every segment" \
		|| notok "a distributed snapshot's view" "$out / $out2"

	# ... and what such a transaction deleted outlives VACUUM on the
	# segments, while a snapshot that hides it is in use: gp_dtx_horizon.
	printf '%s\n' "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT 'established';" \
		"SELECT pg_sleep(3);" "SELECT count(*), sum(b) FROM dtx;" "COMMIT;" |
		qf 0 > "$ROOT/dtx_rr2.out" 2>&1 &
	rr=$!
	sleep 1
	q 0 "DELETE FROM dtx WHERE a > 70;" >/dev/null
	q 0 "VACUUM dtx;" >/dev/null
	wait "$rr"
	out=$(tail -1 "$ROOT/dtx_rr2.out")
	out2=$(q 0 "SELECT count(*) FROM dtx;")
	out3=$(q 1 "SELECT count(*) FROM pg_replication_slots WHERE slot_name = 'gp_dtx_horizon';")
	[ "$out|$out2|$out3" = "80|3240|70|1" ] \
		&& ok "VACUUM on a segment keeps the rows a transaction the snapshot hides deleted" \
		|| notok "the horizon a hidden transaction needs" "$out / $out2 / $out3"
	out=$(q 0 "SELECT sum(result::int) FROM gp.exec_on_segments('SELECT count(*) FROM gp_internal.dtx_map()');")
	[ "$out" = "0" ] && ok "and a segment forgets each transaction once no snapshot can hide it" \
		|| notok "the map of distributed transactions" "$out"

	# Temporary relations: PREPARE is made to take them, and a file dropped
	# with one is unlinked after its second phase.
	out=$(printf '%s\n' "SET client_min_messages = warning;" "BEGIN;" \
		"CREATE TEMP TABLE tdrop (a int) ON COMMIT DROP;" \
		"INSERT INTO tdrop SELECT generate_series(1, 10);" "COMMIT;" \
		"CREATE TEMP TABLE tkeep (a int);" "INSERT INTO tkeep SELECT generate_series(1, 10);" \
		"BEGIN;" "DROP TABLE tkeep;" "COMMIT;" \
		"SELECT sum(result::int) FROM gp.exec_on_segments(\$\$SELECT count(*) FROM pg_ls_dir('base/' || (SELECT oid FROM pg_database WHERE datname = current_database())) f WHERE f LIKE 't%'\$\$);" | qf 0)
	[ "$out" = "0" ] \
		&& ok "temporary tables commit in two phases, and one dropped leaves no file on a segment" \
		|| notok "temporary tables under two-phase commit" "$out"
	# And one TRUNCATE gave new files leaves no old ones: a prepared
	# transaction's record lists no temporary relation's files, and the
	# backend sweeps its own after the second phase.  Each segment has one
	# file number left, its own: a segment numbers a new file itself.
	out=$(printf '%s\n' "SET client_min_messages = warning;" \
		"CREATE TEMP TABLE ttrunc (a int) DISTRIBUTED BY (a);" \
		"INSERT INTO ttrunc SELECT generate_series(1, 100);" "TRUNCATE ttrunc;" \
		"INSERT INTO ttrunc SELECT generate_series(1, 100);" "TRUNCATE ttrunc;" \
		"INSERT INTO ttrunc SELECT generate_series(1, 10);" \
		"SELECT count(*) FROM ttrunc;" \
		"SELECT string_agg(result, ' ' ORDER BY content) FROM gp.exec_on_segments(\$\$SELECT count(DISTINCT split_part(f, '_', 2)) FROM pg_ls_dir('base/' || (SELECT oid FROM pg_database WHERE datname = current_database())) f WHERE f LIKE 't%'\$\$);" | qf 0)
	[ "$out" = "10
1 1" ] \
		&& ok "a temporary table TRUNCATE gave new files leaves no old ones on a segment" \
		|| notok "a temporary table's old files under two-phase commit" "$out"

	# A gid of the distributed kind is the coordinator's.
	out=$(printf '%s\n' "BEGIN;" "PREPARE TRANSACTION 'gp_dtx_12345';" | qf 1)
	case "$out" in
		*'"gp_dtx_12345" is reserved for distributed transactions'*)
			ok "a utility session cannot prepare under a distributed transaction's gid" ;;
		*) notok "a reserved gid" "$out" ;;
	esac

	# The loopback (gp_loopback.c): a storage server made in another database
	# is made in gp.maintenance_database, postgres, by a part of the
	# transaction there that is prepared with it and committed after its
	# commit record, as a segment's part is.
	q 0 "CREATE DATABASE lbdb;" >/dev/null
	ql() { "$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d lbdb -c "$1" 2>&1; }
	qfl() { "$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d lbdb -f - 2>&1; }
	ql "CREATE EXTENSION gp_sql CASCADE;" >/dev/null
	out=$(ql "SELECT gp_sql.create_storage_server('lb_s1', '{\"endpoint\": \"e1\"}');")
	out2=$(q 0 "SELECT srvname || ' ' || array_to_string(srvoptions, ',') FROM pg_foreign_server WHERE srvname = 'lb_s1';")
	out3=$(ql "SELECT count(*) FROM pg_foreign_server;")
	out4=$(ql "SELECT servername FROM gp_sql.storage_servers;")
	[ "$out|$out2|$out3|$out4" = "|lb_s1 endpoint=e1|0|lb_s1" ] \
		&& ok "a storage server made in another database is made in the maintenance database, and read from there" \
		|| notok "a storage server through the loopback" "$out / $out2 / $out3 / $out4"

	q 0 "SELECT gp_inject_fault('loopback_commit_prepared', 'suspend', 1);" >/dev/null
	ql "SELECT gp_sql.create_storage_server('lb_s2');" >/dev/null 2>&1 &
	writer=$!
	q 0 "SELECT gp_wait_until_triggered_fault('loopback_commit_prepared', 1, 1);" >/dev/null
	gid=$(q 0 "SELECT gid FROM pg_prepared_xacts;")
	dbo=$(q 0 "SELECT oid FROM pg_database WHERE datname = 'postgres';")
	x=${gid#gp_dtx_}; x=${x%_*}
	status=$(q 0 "SELECT pg_xact_status('$x'::xid8);" 2>&1)
	seen=$(q 0 "SELECT count(*) FROM pg_foreign_server WHERE srvname = 'lb_s2';")
	q 0 "SELECT gp_inject_fault('loopback_commit_prepared', 'resume', 1);" >/dev/null
	wait "$writer"
	q 0 "SELECT gp_inject_fault('loopback_commit_prepared', 'reset', 1);" >/dev/null
	after=$(q 0 "SELECT count(*) FROM pg_foreign_server WHERE srvname = 'lb_s2';")
	p0=$(q 0 "SELECT count(*) FROM pg_prepared_xacts;")
	case "$gid|$status|$seen|$after|$p0" in
		"gp_dtx_"[0-9]*"_$dbo|in progress|0|1|0")
			ok "its part there is prepared under the coordinator's transaction ID, in progress for the others until that part is committed" ;;
		*) notok "the loopback's two phases" "$gid / $dbo / $status / $seen / $after / $p0" ;;
	esac
	out=$(q 0 "SELECT (SELECT count(*) FROM gp_internal.dtx_map()) || ' ' ||
	                  (SELECT count(*) FROM pg_replication_slots WHERE slot_name = 'gp_dtx_horizon');")
	[ "$out" = "0 0" ] \
		&& ok "the coordinator keeps no map of such parts, nor a slot holding back what they deleted" \
		|| notok "the coordinator's map and slot" "$out"

	ql "CREATE TABLE lbt (a int) DISTRIBUTED BY (a);" >/dev/null
	q 0 "SELECT gp_inject_fault('start_prepare', 'error', $(dbid 2));" >/dev/null
	out=$(printf '%s\n' "BEGIN;" "INSERT INTO lbt SELECT generate_series(1, 10);" \
		"SELECT gp_sql.create_storage_server('lb_s4');" "COMMIT;" | qfl)
	q 0 "SELECT gp_inject_fault('start_prepare', 'reset', $(dbid 2));" >/dev/null
	out2=$(q 0 "SELECT count(*) FROM pg_foreign_server WHERE srvname = 'lb_s4';")
	p0=$(q 0 "SELECT count(*) FROM pg_prepared_xacts;")
	case "$out|$out2|$p0" in
		*"fault triggered, fault name:'start_prepare'"*"|0|0")
			ok "a transaction a segment fails to prepare rolls back its prepared part in the maintenance database" ;;
		*) notok "a failed first phase and the loopback's part" "$out / $out2 / $p0" ;;
	esac

	q 0 "SELECT gp_inject_fault('loopback_commit_prepared', 'panic', 1);" >/dev/null
	ql "SELECT gp_sql.create_storage_server('lb_s3');" >/dev/null 2>&1
	for i in $(seq 1 60); do
		out=$(q 0 "SELECT count(*) FROM pg_foreign_server WHERE srvname = 'lb_s3';" 2>/dev/null)
		[ "$out" = "1" ] && break
		sleep 0.5
	done
	p0=$(q 0 "SELECT count(*) FROM pg_prepared_xacts;")
	log=$(grep -c "distributed transaction recovery: COMMIT PREPARED 'gp_dtx_[0-9]*_[0-9]*' on the coordinator" "$ROOT/node0.log")
	[ "$out|$p0" = "1|0" ] && [ "$log" -ge 1 ] \
		&& ok "a coordinator that went down between them: its recovery process commits that part too" \
		|| notok "recovery of the loopback's part" "$out / $p0 / $log"

	# A storage handler's credentials on a segment: read in the coordinator's
	# maintenance database, as the user (gp_sql's storage.c, through gp_core's
	# loopback).  A server made from another database is the coordinator's
	# alone -- the loopback's statements are not sent on -- so the segments
	# have no copy to read.  gp_storage_probe, a test module, says what a
	# handler would be given.
	ql "CREATE EXTENSION gp_storage_probe;" >/dev/null
	ql "SELECT gp_sql.create_storage_server('cred_srv', '{\"protocol\": \"probe\"}');" >/dev/null
	ql "SELECT gp_sql.create_storage_user_mapping('cred_srv', CURRENT_USER, '{\"secret\": \"sesame\"}');" >/dev/null
	out=$(ql "SELECT string_agg(coalesce(result, 'none'), ',' ORDER BY content)
	            FROM gp.exec_on_segments('SELECT gp_storage_probe.credentials(''cred_srv'')');")
	out2=$(q 1 "SELECT count(*) FROM pg_foreign_server WHERE srvname = 'cred_srv';")
	[ "$out|$out2" = "secret=sesame,secret=sesame|0" ] \
		&& ok "a handler on a segment is given the user's credentials, read on the coordinator" \
		|| notok "credentials on a segment" "$out / $out2"

	# Cloudberry's fault bump_oid, which its tests set on the coordinator to
	# make an object whose OID is past a signed int's: the next OID a catalog
	# row is given moved there, once, the counter left where it was -- and the
	# segments' object is given the same (gp_ddl.c).
	q 0 "SELECT gp_inject_fault('bump_oid', 'skip', 1);" >/dev/null
	q 0 "CREATE TABLE bump_big (a int) DISTRIBUTED BY (a);" >/dev/null
	q 0 "SELECT gp_inject_fault('bump_oid', 'reset', 1);" >/dev/null
	q 0 "CREATE TABLE bump_small (a int) DISTRIBUTED BY (a);" >/dev/null
	out=$(q 0 "SELECT string_agg((oid::bigint > x'7FFFFFFF'::bigint)::text, ' ' ORDER BY relname)
	             FROM pg_class WHERE relname IN ('bump_big', 'bump_small');")
	out2=$(q 0 "SELECT count(*) FROM gp_dist_random('pg_class') WHERE relname = 'bump_big' AND oid = 'bump_big'::regclass;")
	out3=$(q 0 "INSERT INTO bump_big SELECT generate_series(1, 10); SELECT count(*) FROM bump_big;")
	[ "$out|$out2|$out3" = "true false|2|10" ] \
		&& ok "bump_oid gives the next table an OID past a signed int's, on every node, once" \
		|| notok "the fault bump_oid" "$out / $out2 / $out3"

	###########################################################################
	echo "14. the global deadlock detector"
	###########################################################################
	# gp_gdd.c: without it an UPDATE or DELETE of a distributed table locks the
	# table, so that two never wait for each other on different segments;
	# with it rows are locked, and a process on the coordinator breaks the
	# deadlocks that makes, cancelling the younger transaction.
	q 0 "CREATE TABLE gdd (id int, val int) DISTRIBUTED BY (id); INSERT INTO gdd SELECT i, i FROM generate_series(1, 100) i;" >/dev/null
	r0=$(q 0 "SELECT min(id) FROM gdd WHERE gp_segment_id = 0;")
	r1=$(q 0 "SELECT min(id) FROM gdd WHERE gp_segment_id = 1;")

	printf '%s\n' "BEGIN;" "UPDATE gdd SET val = val WHERE id = $r0;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	out=$(q 0 "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND wait_event = 'relation';")
	q 0 "UPDATE gdd SET val = val WHERE id = $r1;" >/dev/null &
	other=$!
	sleep 0.5
	out=$(q 0 "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND wait_event = 'relation';")
	wait "$holder" "$other"
	[ "$out" = "1" ] && ok "without the detector an UPDATE of another row waits for the table, as Cloudberry's does" \
		|| notok "the table lock without the detector" "$out"

	start_node 0 "shared_preload_libraries = '$PRELOAD,gp_orca'" \
		"gp.enable_global_deadlock_detector = on" \
		"gp.global_deadlock_detector_period = 5"
	out=$(q 0 "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'gp_core global deadlock detector';")
	[ "$out" = "1" ] && ok "with gp.enable_global_deadlock_detector the coordinator runs the detector" \
		|| notok "the detector's process" "$out"

	printf '%s\n' "BEGIN;" "UPDATE gdd SET val = val WHERE id = $r0;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	start=$(date +%s%N)
	q 0 "UPDATE gdd SET val = val WHERE id = $r1;" >/dev/null
	elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
	wait "$holder"
	[ "$elapsed" -lt 1000 ] && ok "with it, an UPDATE of another row does not wait ($elapsed ms)" \
		|| notok "row locks with the detector" "$elapsed ms"

	# Cloudberry's gdd/dist-deadlock-01: each holds a row on one segment and
	# waits for the other's on the other.
	printf '%s\n' "BEGIN;" "UPDATE gdd SET val = val WHERE id = $r0;" "SELECT pg_sleep(2);" \
		"UPDATE gdd SET val = val WHERE id = $r1;" "COMMIT;" | qf 0 > "$ROOT/gdd10.out" 2>&1 &
	older=$!
	sleep 0.5
	printf '%s\n' "BEGIN;" "UPDATE gdd SET val = val WHERE id = $r1;" "SELECT pg_sleep(1);" \
		"UPDATE gdd SET val = val WHERE id = $r0;" "COMMIT;" | qf 0 > "$ROOT/gdd20.out" 2>&1 &
	younger=$!
	wait "$older" "$younger"
	out=$(grep -c ERROR "$ROOT/gdd10.out")
	out2=$(grep ERROR "$ROOT/gdd20.out")
	log=$(grep -c "global deadlock detected" "$ROOT/node0.log")
	case "$out|$out2|$log" in
		'0|'*'ERROR:  canceling statement due to user request: "cancelled by global deadlock detector"|'[1-9]*)
			ok "a deadlock across two segments is broken: the younger transaction is cancelled, in Cloudberry's words" ;;
		*) notok "a distributed deadlock" "$out / $out2 / $log" ;;
	esac

	# Commit ordering (gp_dtx.c): with rows locked, an UPDATE may update the
	# row a one-phase part has committed on its segment before that part's
	# coordinator transaction has ended.  It ends only after that one -- a
	# snapshot that saw it committed and the other in progress would show
	# both the row it replaced and its own.  The first is held after its
	# segment committed, as its commit record is written here.
	val=$(q 0 "SELECT val FROM gdd WHERE id = $r0;")
	q 0 "SELECT gp_inject_fault('onephase_transaction_commit', 'suspend', 1);" >/dev/null
	q 0 "UPDATE gdd SET val = val + 1 WHERE id = $r0;" >/dev/null 2>&1 &
	first=$!
	q 0 "SELECT gp_wait_until_triggered_fault('onephase_transaction_commit', 1, 1);" >/dev/null
	q 0 "UPDATE gdd SET val = val + 10 WHERE id = $r0;" >/dev/null 2>&1 &
	second=$!
	waits=
	for _ in $(seq 150); do
		waits=$(q 0 "SELECT wait_event FROM pg_stat_activity WHERE query LIKE 'UPDATE gdd SET val = val + 10 %';")
		[ "$waits" = transactionid ] && break
		sleep 0.2
	done
	seen=$(q 0 "SELECT string_agg(val::text, ',') FROM gdd WHERE id = $r0;")
	q 0 "SELECT gp_inject_fault('onephase_transaction_commit', 'resume', 1);" >/dev/null
	wait "$first" "$second"
	q 0 "SELECT gp_inject_fault('onephase_transaction_commit', 'reset', 1);" >/dev/null
	after=$(q 0 "SELECT string_agg(val::text, ',') FROM gdd WHERE id = $r0;")
	[ "$waits|$seen|$after" = "transactionid|$val|$((val + 11))" ] \
		&& ok "an UPDATE of a row a one-phase commit wrote ends after it, and no snapshot sees the row twice" \
		|| notok "commit ordering after a one-phase commit" "$waits / $seen / $after (was $val)"

	# The planner's Split without the cluster secret -- an UPDATE of the
	# distribution key the coordinator carries out, a DELETE ... RETURNING
	# where the row is and an INSERT where it hashes (gp_explicit.c) -- of
	# a row another transaction changes meanwhile: its DELETE comes short of
	# the row, and asks the segment why (explicit_recheck(), gp_split.c).
	# It was lost: the old version stayed, and nothing was moved.  Now it is
	# refused in Cloudberry's words, as the Split with the secret refuses it.
	q 0 "CREATE TABLE gdd_k (id int, val int) DISTRIBUTED BY (id); INSERT INTO gdd_k VALUES ($r0, 0), ($r1, 0);
	     CREATE TABLE gdd_s (id int) DISTRIBUTED RANDOMLY; INSERT INTO gdd_s VALUES ($r0), ($r1);" >/dev/null
	printf '%s\n' "BEGIN;" "UPDATE gdd_k SET val = val + 1 WHERE id = $r0;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	out=$(q 0 "SET gp.optimizer = off; UPDATE gdd_k SET id = gdd_k.id + 1000 FROM gdd_s WHERE gdd_k.id = gdd_s.id AND gdd_s.id = $r0;")
	wait "$holder"
	out2=$(q 0 "SELECT id, val FROM gdd_k WHERE id IN ($r0, $r0 + 1000);")
	case "$out|$out2" in
		*"EvalPlanQual can not handle subPlan with Motion node"*"|$r0|1")
			ok "without the secret a Split of a row updated meanwhile is refused, and the update stands" ;;
		*) notok "a Split of a row updated meanwhile, without the secret" "$out / $out2" ;;
	esac
	printf '%s\n' "BEGIN;" "DELETE FROM gdd_k WHERE id = $r1;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	out=$(q 0 "SET gp.optimizer = off; UPDATE gdd_k SET id = gdd_k.id + 1000 FROM gdd_s WHERE gdd_k.id = gdd_s.id AND gdd_s.id = $r1;")
	wait "$holder"
	out2=$(q 0 "SELECT count(*) FROM gdd_k WHERE id IN ($r1, $r1 + 1000);")
	case "$out|$out2" in
		*"could not split update tuple which has been deleted by other transaction"*"|0")
			ok "and so is one deleted meanwhile, as Cloudberry refuses it" ;;
		*) notok "a Split of a row deleted meanwhile, without the secret" "$out / $out2" ;;
	esac
	q 0 "DROP TABLE gdd_k, gdd_s;" >/dev/null

	# SELECT ... FOR UPDATE (lockrows.c, gp_modify.c): without the detector
	# Cloudberry's table lock, with it the rows, locked on the segments --
	# by the planner's gather and by ORCA's LockRows in the Gather's
	# fragment -- for the query Cloudberry's planner locks the rows of.  With
	# the cluster secret, so that ORCA's plans are dispatched.
	SECRET="cluster-secret-$RANDOM$RANDOM$RANDOM"
	for n in 1 2 0; do
		start_node "$n" "shared_preload_libraries = '$PRELOAD,gp_orca'" \
			"gp.cluster_secret = '$SECRET'" \
			"gp.enable_global_deadlock_detector = on"
	done
	out=$(q 0 "SET gp.optimizer = on; EXPLAIN (COSTS OFF) SELECT * FROM gdd WHERE val < 5 ORDER BY id FOR UPDATE;" | tr '\n' '|')
	case "$out" in
		*"Gather Motion"*"Merge Key"*"LockRows"*"Sort"*"Seq Scan on gdd"*)
			ok "ORCA locks the rows below the Gather, above the sort its merge keeps" ;;
		*) notok "ORCA's plan for FOR UPDATE" "$out" ;;
	esac
	# sorted by a column it does not return, a junk column ORCA's plan leaves out
	out=$(q 0 "SET gp.optimizer = on; EXPLAIN (COSTS OFF) SELECT id FROM gdd WHERE val < 5 ORDER BY -val FOR UPDATE;" | tr '\n' '|')
	out2=$(q 0 "SET gp.optimizer = on; SELECT id FROM gdd WHERE val < 5 ORDER BY -val FOR UPDATE;" | tr '\n' ',')
	out3=$(q 0 "SET gp.optimizer = off; SELECT id FROM gdd WHERE val < 5 ORDER BY -val FOR UPDATE;" | tr '\n' ',')
	case "$out|$out2" in
		*"LockRows"*"Optimizer: GPORCA"*"|$out3")
			[ -n "$out3" ] && ok "and sorted by a column it does not return, one column, as the planner's" \
				|| notok "ORCA's FOR UPDATE sorted by a column it does not return" "no rows" ;;
		*) notok "ORCA's FOR UPDATE sorted by a column it does not return" "$out / $out2 / $out3" ;;
	esac

	# With rows locked, an UPDATE that waited for another's update of its row
	# re-checks the row's new version (EvalPlanQual), running the plan below
	# its write again: ORCA's, on the segment, acts on the new version.  One
	# that reads another table re-checks it with the row of it the changed
	# one was joined to, fetched again by its ctid through a row mark, as the
	# planner's does (the translator's AddOtherRowMarks): on the segment,
	# where the tables are distributed alike and no Motion is below the
	# write.  A Motion below the write could not run again for one row, and
	# fails the recheck as Cloudberry's does (gp_motion.c).
	q 0 "CREATE TABLE gddr (a int, b int) DISTRIBUTED RANDOMLY; INSERT INTO gddr VALUES (1, 1);" >/dev/null
	plan=$(q 0 "EXPLAIN (COSTS OFF) UPDATE gddr SET b = b + 10 WHERE a = 1;")
	printf '%s\n' "BEGIN;" "UPDATE gddr SET b = b + 1 WHERE a = 1;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	out=$(q 0 "UPDATE gddr SET b = b + 10 WHERE a = 1 RETURNING b;")
	wait "$holder"
	case "$plan|$out" in
		*"Update on gddr"*"Optimizer: GPORCA"*"|12")
			ok "with it, ORCA's UPDATE that waited acts on the row's new version" ;;
		*) notok "EvalPlanQual under ORCA with the detector" "$plan / $out" ;;
	esac
	q 0 "CREATE TABLE gddj (id int, w int) DISTRIBUTED BY (id); INSERT INTO gddj SELECT id, id * 100 FROM gdd;" >/dev/null
	plan=$(q 0 "EXPLAIN (COSTS OFF, VERBOSE) UPDATE gdd SET val = gdd.val + gddj.w FROM gddj WHERE gdd.id = gddj.id AND gdd.id = $r0;")
	before=$(q 0 "SELECT val FROM gdd WHERE id = $r0;")
	printf '%s\n' "BEGIN;" "UPDATE gdd SET val = val + 1 WHERE id = $r0;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	out=$(q 0 "UPDATE gdd SET val = gdd.val + gddj.w FROM gddj WHERE gdd.id = gddj.id AND gdd.id = $r0 RETURNING gdd.val;")
	wait "$holder"
	printf '%s\n' "BEGIN;" "UPDATE gdd SET val = val WHERE id = 1;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	out2=$(q 0 "UPDATE gdd SET val = val FROM gddr WHERE gdd.id = gddr.a;")
	wait "$holder"
	case "$plan|$out|$out2" in
		*"gddj.ctid"*"Optimizer: GPORCA"*"|$((before + 1 + r0 * 100))|"*"EvalPlanQual can not handle subPlan with Motion node"*)
			ok "... and one that joins a table distributed alike, with the row it was joined to; through a Motion, Cloudberry's error" ;;
		*) notok "EvalPlanQual of ORCA's joined UPDATE with the detector" "$plan / $before / $out / $out2" ;;
	esac
	# ... and one whose WHERE has an EXISTS over it: ORCA's semi-join, which
	# returns the row of it the changed one matched, carried up for its row
	# mark as the planner's semi-join carries it.
	plan=$(q 0 "EXPLAIN (COSTS OFF, VERBOSE) UPDATE gdd SET val = gdd.val * 2 WHERE EXISTS (SELECT 1 FROM gddj WHERE gddj.id = gdd.id AND gddj.w > 0) AND gdd.id = $r0;")
	before=$(q 0 "SELECT val FROM gdd WHERE id = $r0;")
	printf '%s\n' "BEGIN;" "UPDATE gdd SET val = val + 1 WHERE id = $r0;" "SELECT pg_sleep(2);" "COMMIT;" |
		qf 0 >/dev/null 2>&1 &
	holder=$!
	sleep 0.5
	out=$(q 0 "UPDATE gdd SET val = gdd.val * 2 WHERE EXISTS (SELECT 1 FROM gddj WHERE gddj.id = gdd.id AND gddj.w > 0) AND gdd.id = $r0 RETURNING gdd.val;")
	wait "$holder"
	case "$plan|$out" in
		*"Semi Join"*"gddj.ctid"*"Optimizer: GPORCA"*"|$(( (before + 1) * 2 ))")
			ok "... and one whose EXISTS reads it, a semi-join, with the row it matched" ;;
		*) notok "EvalPlanQual of ORCA's UPDATE with an EXISTS, with the detector" "$plan / $before / $out" ;;
	esac
	for opt in off on; do
		printf '%s\n' "SET gp.optimizer = $opt;" "BEGIN;" "SELECT id FROM gdd WHERE id = $r0 FOR UPDATE;" \
			"SELECT string_agg(mode, ',' ORDER BY mode) FROM pg_locks WHERE relation = 'gdd'::regclass;" \
			"SELECT pg_sleep(2);" "COMMIT;" | qf 0 > "$ROOT/gddfu.out" 2>&1 &
		holder=$!
		sleep 0.5
		out=$(q 0 "SET gp.optimizer = $opt; SELECT id FROM gdd WHERE id = $r0 FOR UPDATE NOWAIT;")
		start=$(date +%s%N)
		q 0 "UPDATE gdd SET val = val WHERE id = $r1;" >/dev/null
		other=$(( ($(date +%s%N) - start) / 1000000 ))
		start=$(date +%s%N)
		q 0 "UPDATE gdd SET val = val WHERE id = $r0;" >/dev/null
		locked=$(( ($(date +%s%N) - start) / 1000000 ))
		wait "$holder"
		out2=$(grep -m1 "Lock" "$ROOT/gddfu.out")
		# The table's own lock is AccessShareLock from the parser, which
		# cannot tell yet whether the rows can be locked on the segments,
		# and RowShareLock from planning, which can (O30), as Cloudberry's
		# parser holds the two.
		case "$out|$out2" in
			*"could not obtain lock on row"*"|AccessShareLock,RowShareLock")
				[ "$other" -lt 1000 ] && [ "$locked" -ge 1000 ] \
					&& ok "with it, FOR UPDATE under gp.optimizer = $opt locks one row on its segment: NOWAIT fails there, another row's UPDATE passes, its own waits" \
					|| notok "row locks under gp.optimizer = $opt" "$other ms / $locked ms" ;;
			*) notok "row locks under gp.optimizer = $opt" "$out / $out2" ;;
		esac
	done

	# A join's rows cannot be locked on the segments: the tables' lock,
	# ExclusiveLock, taken as the query is planned -- after AccessShareLock,
	# not RowShareLock, so not an upgrade two sessions deadlock on -- and
	# recorded in the plan, so that a cached plan takes it too.
	q 0 "CREATE TABLE gdd2 (id int, val int) DISTRIBUTED BY (id); INSERT INTO gdd2 SELECT i, i FROM generate_series(1, 100) i;" >/dev/null
	for opt in off on; do
		out=$(printf '%s\n' "SET gp.optimizer = $opt;" "BEGIN;" \
			"SELECT g.id FROM gdd g JOIN gdd2 h USING (id) WHERE g.id = $r0 FOR UPDATE;" \
			"SELECT string_agg(mode, ',' ORDER BY mode) FROM pg_locks WHERE relation = 'gdd'::regclass;" \
			"COMMIT;" | qf 0 | tail -1)
		[ "$out" = "AccessShareLock,ExclusiveLock" ] \
			&& ok "with it, a join FOR UPDATE under gp.optimizer = $opt locks the tables as it is planned, after AccessShareLock" \
			|| notok "a join's FOR UPDATE under gp.optimizer = $opt" "$out"
		out=$(printf '%s\n' "SET gp.optimizer = $opt;" \
			"PREPARE gddj AS SELECT g.id FROM gdd g JOIN gdd2 h USING (id) WHERE g.id = $r0 FOR UPDATE;" \
			"EXECUTE gddj;" "BEGIN;" "EXECUTE gddj;" \
			"SELECT string_agg(mode, ',' ORDER BY mode) FROM pg_locks WHERE relation = 'gdd'::regclass;" \
			"COMMIT;" | qf 0 | tail -1)
		[ "$out" = "AccessShareLock,ExclusiveLock" ] \
			&& ok "and a cached plan of it takes the ExclusiveLock again, under gp.optimizer = $opt" \
			|| notok "a cached plan's lock for a join's FOR UPDATE under gp.optimizer = $opt" "$out"
	done

	for n in 1 2 0; do
		start_node "$n" "shared_preload_libraries = '$PRELOAD,gp_orca'" \
			"gp.cluster_secret = '$SECRET'"
	done
	out=$(printf '%s\n' "SET gp.optimizer = on;" "BEGIN;" "SELECT id FROM gdd WHERE id = $r0 FOR UPDATE;" \
		"SELECT string_agg(mode, ',' ORDER BY mode) FROM pg_locks WHERE relation = 'gdd'::regclass;" \
		"COMMIT;" | qf 0 | tail -1)
	# ExclusiveLock alone: the parser opened the table in it (O30), as
	# Cloudberry's parser does, not in RowShareLock that it then upgraded.
	[ "$out" = "ExclusiveLock" ] \
		&& ok "without it, ORCA's FOR UPDATE takes Cloudberry's table lock, ExclusiveLock, as the parser opens the table" \
		|| notok "the table lock for FOR UPDATE without the detector" "$out"

	# A write through a view: the rewriter brings the table in, and locks it
	# in ExclusiveLock (O30), not in RowExclusiveLock that the executor's
	# ExclusiveLock would then upgrade.
	q 0 "CREATE VIEW gddv AS SELECT id, val FROM gdd;" >/dev/null
	out=$(printf '%s\n' "BEGIN;" "UPDATE gddv SET val = val WHERE id = $r0;" \
		"SELECT string_agg(mode, ',' ORDER BY mode) FROM pg_locks WHERE relation = 'gdd'::regclass;" \
		"COMMIT;" | qf 0 | tail -1)
	case ",$out," in
		*,ExclusiveLock,*,RowExclusiveLock,*|*,RowExclusiveLock,*) notok "the table under a view without the detector" "$out" ;;
		*,ExclusiveLock,*) ok "without it, an UPDATE through a view locks the table in ExclusiveLock, as the rewriter brings it in" ;;
		*) notok "the table under a view without the detector" "$out" ;;
	esac

	# A MERGE that only inserts changes no row it reads, and locks as an
	# INSERT does: RowExclusiveLock alone, which the parser took for INSERT
	# privilege -- not an ExclusiveLock after it, an upgrade two such MERGEs
	# deadlock on -- and one waits for no other.  A MERGE that updates holds
	# ExclusiveLock, from the parser.
	q 0 "CREATE TABLE gddm (id int, val int) DISTRIBUTED BY (id);" >/dev/null
	for opt in off on; do
		q 0 "TRUNCATE gddm;" >/dev/null
		out=$(printf '%s\n' "SET gp.optimizer = $opt;" "BEGIN;" \
			"MERGE INTO gddm t USING (VALUES (1, 1), (2, 2)) s(id, val) ON t.id = s.id WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.val);" \
			"SELECT string_agg(mode, ',' ORDER BY mode) FROM pg_locks WHERE relation = 'gddm'::regclass AND pid = pg_backend_pid();" \
			"COMMIT;" | qf 0 | tail -1)
		out2=$(printf '%s\n' "SET gp.optimizer = $opt;" "BEGIN;" \
			"MERGE INTO gddm t USING (VALUES (1, 10), (3, 3)) s(id, val) ON t.id = s.id WHEN MATCHED THEN UPDATE SET val = s.val WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.val);" \
			"SELECT string_agg(mode, ',' ORDER BY mode) FROM pg_locks WHERE relation = 'gddm'::regclass AND pid = pg_backend_pid();" \
			"COMMIT;" | qf 0 | tail -1)
		printf '%s\n' "SET gp.optimizer = $opt;" "BEGIN;" \
			"MERGE INTO gddm t USING (VALUES (4, 4)) s(id, val) ON t.id = s.id WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.val);" \
			"SELECT pg_sleep(3);" "COMMIT;" | qf 0 > /dev/null 2>&1 &
		holder=$!
		sleep 0.5
		start=$(date +%s%N)
		out3=$(q 0 "SET gp.optimizer = $opt; MERGE INTO gddm t USING (VALUES (5, 5)) s(id, val) ON t.id = s.id WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.val);")
		took=$(( ($(date +%s%N) - start) / 1000000 ))
		wait "$holder"
		n=$(q 0 "SELECT string_agg(id || ':' || val, ' ' ORDER BY id) FROM gddm;")
		[ "$out" = "RowExclusiveLock" ] && [ "$out2" = "ExclusiveLock" ] && [ "$took" -lt 2000 ] &&
			[ "$n" = "1:10 2:2 3:3 4:4 5:5" ] \
			&& ok "without it, a MERGE that only inserts locks as an INSERT does, and waits for no other; one that updates holds ExclusiveLock, under gp.optimizer = $opt" \
			|| notok "a MERGE's table lock without the detector, under gp.optimizer = $opt" "$out / $out2 / $took ms / $n / $out3"
	done

	for n in 1 2 0; do
		start_node "$n" "shared_preload_libraries = '$PRELOAD,gp_orca'"
	done

	###########################################################################
	echo "15. the segments authenticate the coordinator, with SCRAM"
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

	###########################################################################
	echo "16. the nodes authenticate each other by certificates"
	###########################################################################
	# Decision 5 asks for certificates in production.  Each node has one of
	# the cluster's authority, which it shows as a server and as a client:
	# the segments take it for any role (cert map=gpnodes, pg_ident.conf's
	# "all"), over TCP, where TLS is -- a socket has none -- and the
	# coordinator connects with gp.internal_sslmode = verify-full, its
	# certificate and the authority's, on every connection it makes: the
	# gang, its readers, distributed transaction recovery, the detector.
	certs="$ROOT/certs"
	mkdir -p "$certs"
	mkcert() {				# mkcert <name> <authority> <CN>
		openssl req -new -nodes -newkey rsa:2048 -subj "/CN=$3" \
			-keyout "$certs/$1.key" -out "$certs/$1.csr" &&
		openssl x509 -req -in "$certs/$1.csr" -days 2 -CA "$certs/$2.crt" \
			-CAkey "$certs/$2.key" -CAcreateserial -out "$certs/$1.crt" \
			-extfile <(printf 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1\n')
	}
	if openssl req -new -x509 -nodes -newkey rsa:2048 -days 2 -subj "/CN=cluster-ca" \
			-keyout "$certs/ca.key" -out "$certs/ca.crt" &&
		openssl req -new -x509 -nodes -newkey rsa:2048 -days 2 -subj "/CN=other-ca" \
			-keyout "$certs/other.key" -out "$certs/other.crt" &&
		mkcert node ca cloudberry-node && mkcert rogue other cloudberry-node
	then
		chmod 600 "$certs"/*.key
		ok "the cluster's authority, a node's certificate, and another authority's"
	else
		notok "openssl makes the certificates"
	fi >"$ROOT/openssl.log" 2>&1
	tail -1 "$ROOT/openssl.log"

	# The same nodes, named by host and port: TLS is TCP's.
	TCPCONF="$ROOT/gp_cluster_tcp.conf"
	{
		echo "# dbid content role host port datadir"
		for n in 0 1 2; do
			echo "$(dbid "$n") $(content "$n") p localhost $(port "$n") $(datadir "$n")"
		done
	} > "$TCPCONF"
	TLS=("gp.cluster_config = '$TCPCONF'" "listen_addresses = 'localhost'"
		 "ssl = on" "ssl_cert_file = '$certs/node.crt'"
		 "ssl_key_file = '$certs/node.key'" "ssl_ca_file = '$certs/ca.crt'"
		 "gp.cluster_secret = '$SECRET'")
	for n in 1 2; do
		cp "$(datadir "$n")/pg_hba.conf" "$ROOT/pg_hba.$n"
		sed -i -E 's/^host(\s+all\s+all\s+\S+\s+)trust$/hostssl\1cert map=gpnodes/' \
			"$(datadir "$n")/pg_hba.conf"
		echo "gpnodes cloudberry-node all" >> "$(datadir "$n")/pg_ident.conf"
		start_node "$n" "${TLS[@]}"
	done

	"$BINDIR/pg_ctl" -D "$(datadir 0)" -m immediate stop >/dev/null 2>&1
	logsize=$(stat -c %s "$ROOT/node0.log")
	start_node 0 "${TLS[@]}"
	out=$(q 0 "SELECT count(*) FROM gp.exec_on_segments('SELECT 1');")
	case "$out" in
		*"certificate"*) ok "a coordinator without its certificate is refused by the segments" ;;
		*) notok "a segment should have asked for a certificate" "$out" ;;
	esac
	out=0
	for i in $(seq 1 20); do
		out=$(tail -c +"$((logsize + 1))" "$ROOT/node0.log" |
			grep -c "distributed transaction recovery could not connect")
		[ "$out" -gt 0 ] && break
		sleep 0.5
	done
	[ "$out" -gt 0 ] && ok "and so is its distributed transaction recovery, which connects as it starts" \
		|| notok "recovery should have failed to connect without the certificate"

	CLIENT=("gp.internal_sslmode = 'verify-full'"
			"gp.internal_sslrootcert = '$certs/ca.crt'")
	"$BINDIR/pg_ctl" -D "$(datadir 0)" -m immediate stop >/dev/null 2>&1
	logsize=$(stat -c %s "$ROOT/node0.log")
	start_node 0 "${TLS[@]}" "${CLIENT[@]}" \
		"gp.internal_sslcert = '$certs/node.crt'" "gp.internal_sslkey = '$certs/node.key'" \
		"shared_preload_libraries = '$PRELOAD,gp_orca'" \
		"gp.enable_global_deadlock_detector = on" "gp.global_deadlock_detector_period = 5"
	out=$(q 0 "SELECT DISTINCT result FROM gp.exec_on_segments('SELECT ssl::text || '' '' || client_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid()');")
	[ "$out" = "true /CN=cloudberry-node" ] \
		&& ok "with it, the dispatcher's connections are TLS, the node's certificate its client's" \
		|| notok "the dispatcher's connection to a segment over TLS" "$out"

	# A join of two redistributed sides is a slice on a reader of each
	# segment; the INSERT writes on both segments, so it commits in two
	# phases, on the gang's connections.
	out=$(printf '%s\n' "SET gp.optimizer = on;" \
		"CREATE TABLE tls_t (a int, b int) DISTRIBUTED BY (a);" \
		"INSERT INTO tls_t SELECT i, i % 7 FROM generate_series(1, 100) i;" \
		"SELECT count(*) FROM tls_t t1 JOIN tls_t t2 ON t1.b = t2.a;" \
		"SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM pg_stat_activity JOIN pg_stat_ssl USING (pid) WHERE application_name = ''cloudberry reader'' AND ssl AND client_dn = ''/CN=cloudberry-node''') WHERE result::int > 0;" \
		"DROP TABLE tls_t;" | qf 0)
	[ "$out" = "$(printf '86\n2')" ] \
		&& ok "a join's readers connect with it too, and the INSERT's two phases commit over TLS" \
		|| notok "readers and two-phase commit over TLS" "$out"

	out=""
	for i in $(seq 1 30); do
		out=$(q 0 "SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM pg_stat_activity JOIN pg_stat_ssl USING (pid) WHERE application_name = ''cloudberry global deadlock detector'' AND ssl') WHERE result::int > 0;")
		[ "$out" = "2" ] && break
		sleep 0.5
	done
	[ "$out" = "2" ] && ok "the deadlock detector's connections to the segments are TLS" \
		|| notok "the detector's connections over TLS" "$out"

	out=$(tail -c +"$((logsize + 1))" "$ROOT/node0.log" | grep -c "could not connect")
	[ "$out" = "0" ] && ok "distributed transaction recovery reaches every segment as the coordinator starts" \
		|| notok "the coordinator's processes could not connect" \
			"$(tail -c +"$((logsize + 1))" "$ROOT/node0.log" | grep "could not connect" | head -2)"

	start_node 0 "${TLS[@]}" "${CLIENT[@]}" \
		"gp.internal_sslcert = '$certs/rogue.crt'" "gp.internal_sslkey = '$certs/rogue.key'"
	seglog=$(stat -c %s "$ROOT/node1.log")
	out=$(q 0 "SELECT count(*) FROM gp.exec_on_segments('SELECT 1');")
	case "$out" in
		*"failed to acquire resources on one or more segments"*"SSL error"*)
			if tail -c +"$((seglog + 1))" "$ROOT/node1.log" | grep -q "could not accept SSL connection: certificate verify failed"; then
				ok "a certificate of another authority is refused, its name though the node's"
			else
				notok "the segment should have refused the certificate itself" \
					"$(tail -c +"$((seglog + 1))" "$ROOT/node1.log" | tail -2)"
			fi ;;
		*) notok "another authority's certificate should be refused" "$out" ;;
	esac

	start_node 0 "${TLS[@]}" "gp.internal_sslmode = 'verify-full'" \
		"gp.internal_sslrootcert = '$certs/other.crt'" \
		"gp.internal_sslcert = '$certs/node.crt'" "gp.internal_sslkey = '$certs/node.key'"
	out=$(q 0 "SELECT count(*) FROM gp.exec_on_segments('SELECT 1');")
	case "$out" in
		*"certificate verify failed"*) ok "and the coordinator refuses a segment its authority did not sign" ;;
		*) notok "the coordinator should have refused the segments' certificate" "$out" ;;
	esac

	out=$(q 0 "SET gp.internal_sslmode = 'verify-everything';" 2>&1)
	case "$out" in
		*"invalid value"*) ok "gp.internal_sslmode takes libpq's modes alone" ;;
		*) notok "gp.internal_sslmode's check" "$out" ;;
	esac

	# Back to the sockets, trust and no TLS.
	for n in 1 2; do
		cp "$ROOT/pg_hba.$n" "$(datadir "$n")/pg_hba.conf"
		start_node "$n"
	done
	start_node 0

	###########################################################################
	echo "17. gp_toolkit, Cloudberry's views of the cluster"
	###########################################################################
	q 0 "CREATE TABLE tk (a int, b int) DISTRIBUTED BY (a);
		 INSERT INTO tk SELECT i, i FROM generate_series(1, 1000) i;
		 CREATE TABLE tkr (a int) DISTRIBUTED REPLICATED;
		 INSERT INTO tkr SELECT generate_series(1, 100);" >/dev/null
	want=$(q 0 "SELECT string_agg(gp_segment_id || ':' || n, ' ' ORDER BY gp_segment_id)
				FROM (SELECT gp_segment_id, count(*) AS n FROM tk GROUP BY 1) s;")
	out=$(q 0 "SELECT string_agg(segid || ':' || segtupcount, ' ' ORDER BY segid)
			   FROM gp_toolkit.gp_skew_details('tk'::regclass);")
	[ -n "$want" ] && [ "$out" = "$want" ] \
		&& ok "gp_skew_details counts a table's rows on each segment ($out)" \
		|| notok "gp_skew_details" "want [$want] got [$out]"
	out=$(q 0 "SELECT string_agg(segid || ':' || segtupcount, ' ' ORDER BY segid)
			   FROM gp_toolkit.gp_skew_details('tkr'::regclass);")
	[ "$out" = "0:100 1:100" ] && ok "and a replicated table's on each, the same" \
		|| notok "gp_skew_details of a replicated table" "$out"
	out=$(q 0 "SELECT (skccoeff < 20)::text || ' ' || (siffraction < 0.2)::text
			   FROM gp_toolkit.gp_skew_coefficient('tk'::regclass),
					gp_toolkit.gp_skew_idle_fraction('tk'::regclass);")
	[ "$out" = "true true" ] && ok "and its skew coefficient and idle fraction" \
		|| notok "gp_skew_coefficient and gp_skew_idle_fraction" "$out"
	it=$(q 0 "SHOW gp.interconnect_type;")
	out=$(q 0 "SELECT string_agg(paramsegment || ':' || paramname || '=' || paramvalue, ' '
									ORDER BY paramsegment)
			   FROM gp_toolkit.gp_param_setting('gp_interconnect_type');")
	[ "$out" = "-1:gp_interconnect_type=$it 0:gp_interconnect_type=$it 1:gp_interconnect_type=$it" ] \
		&& ok "gp_param_setting gives a setting by Cloudberry's name, on the coordinator and each segment" \
		|| notok "gp_param_setting" "$out"
	out=$(q 0 "SELECT string_agg(paramsegment || ':' || paramvalue, ' ' ORDER BY paramsegment)
			   FROM gp_toolkit.gp_param_settings() WHERE paramname = 'gp.dbid';")
	[ "$out" = "0:$(dbid 1) 1:$(dbid 2)" ] \
		&& ok "gp_param_settings gives each segment's settings, run there" \
		|| notok "gp_param_settings" "$out"
	out=$(q 0 "SELECT count(*) FROM gp_toolkit.gp_param_settings_seg_value_diffs
			   WHERE psdname IN ('gp.dbid', 'gp.qe_identity', 'hosts_file', 'port');")
	[ "$out" = "0" ] && ok "and gp_param_settings_seg_value_diffs leaves out what is each node's own" \
		|| notok "gp_param_settings_seg_value_diffs" "$out"
	out=$(q 0 "SELECT pg_catalog.gp_execution_segment() || ' ' ||
					  (SELECT string_agg(s::text, ' ' ORDER BY s)
					   FROM (SELECT pg_catalog.gp_execution_segment() AS s
							 FROM gp_dist_random('gp_id')) d);")
	[ "$out" = "-1 0 1" ] && ok "gp_execution_segment() is the content id of the node the call runs on" \
		|| notok "gp_execution_segment()" "$out"
	out=$(q 0 "SELECT count(*) || ' ' || count(*) FILTER (WHERE valid) FROM pg_catalog.gp_pgdatabase;
			   SELECT count(*) FROM gp_toolkit.gp_pgdatabase_invalid;")
	[ "$out" = "3 3
0" ] && ok "gp_pgdatabase has every node, valid, and gp_pgdatabase_invalid none" \
		|| notok "gp_pgdatabase" "$out"
	out=$(q 0 "SELECT count(*) FROM gp_toolkit.gp_stats_missing WHERE smitable = 'tk';
			   ANALYZE tk;
			   SELECT count(*) FROM gp_toolkit.gp_stats_missing WHERE smitable = 'tk';")
	[ "$out" = "1
0" ] && ok "gp_stats_missing lists a table until it is analyzed" \
		|| notok "gp_stats_missing" "$out"
	out=$(q 0 "SELECT (sotdsize = pg_relation_size('tk'))::text || ' ' || (sotdsize > 0)::text
			   FROM gp_toolkit.gp_size_of_table_disk WHERE sotdtablename = 'tk';
			   SELECT (sosdschematablesize >= pg_relation_size('tk'))::text
			   FROM gp_toolkit.gp_size_of_schema_disk WHERE sosdnsp = 'public';")
	[ "$out" = "true true
true" ] && ok "gp_size_of_table_disk and gp_size_of_schema_disk, the cluster's sizes" \
		|| notok "gp_toolkit's size views" "$out"
	out=$(q 0 "SELECT count(*) FILTER (WHERE iaotype)
			   FROM gp_toolkit.__gp_is_append_only JOIN pg_class ON oid = iaooid
			   WHERE relname IN ('tk', 'tkr');
			   DROP TABLE tk, tkr;")
	[ "$out" = "0" ] && ok "__gp_is_append_only: no heap table is" \
		|| notok "__gp_is_append_only" "$out"

	# Cloudberry's own log, which gp_core writes beside PostgreSQL's in each
	# node's log directory (gp_log.c), and gp_toolkit's views of it.  An
	# error here, its statement with it and again in the line after it, and
	# no statement where log_min_error_statement leaves it out.
	n=0
	for d in 0 1 2; do
		ls "$(datadir "$d")/log" 2>/dev/null | grep -q '^gpdb-.*\.csv$' && n=$((n + 1))
	done
	printf '%s\n' "SELECT 1 FROM gp_log_nowhere_1;" "SET log_min_error_statement = panic;" \
		"SELECT 1 FROM gp_log_nowhere_2;" | qf 0 >/dev/null
	out=$(q 0 "SELECT string_agg(logseverity || '|' || logmessage || '|' || coalesce(logdebug, '') || '|' ||
								 logsegment || '|' || (logsession ~ '^con[0-9]+$') || (logcmdcount ~ '^cmd[0-9]+$'),
								 E'\n' ORDER BY logtime)
			   FROM gp_toolkit.__gp_log_coordinator_ext
			   WHERE logmessage LIKE '%gp\\_log\\_nowhere\\_%' AND logdatabase = 'postgres';")
	[ "$n" = "3" ] && [ "$out" = 'ERROR|relation "gp_log_nowhere_1" does not exist|SELECT 1 FROM gp_log_nowhere_1;|seg-1|truetrue
LOG|An exception was encountered during the execution of statement: SELECT 1 FROM gp_log_nowhere_1;|SELECT 1 FROM gp_log_nowhere_1;|seg-1|truetrue
ERROR|relation "gp_log_nowhere_2" does not exist||seg-1|truetrue' ] \
		&& ok "each node's log/gpdb-*.csv: an error's record, its statement's, and none where log_min_error_statement says" \
		|| notok "Cloudberry's log on the coordinator" "$n files / $out"

	# A segment's error names the client's statement, as Cloudberry's
	# segment names the statement it was dispatched: the one a gather's
	# query comes with, its comments' ends and backslashes as they were.
	q 0 "CREATE TABLE lg (a int, b text) DISTRIBUTED BY (a);
		 INSERT INTO lg SELECT i, 'x' FROM generate_series(1, 100) i;" >/dev/null
	seg=$(q 0 "SELECT 'seg' || gp_segment_id FROM lg WHERE a = 5;")
	stmt="SELECT * /* a */ FROM lg WHERE a = 5 AND 1 / (a - a) = 1 AND b <> E'\\\\*/';"
	out=$(q 0 "$stmt" 2>&1)
	out2=$(q 0 "SELECT string_agg(logsegment || '|' || (logdebug = \$s\$$stmt\$s\$), ' ')
				FROM gp_toolkit.__gp_log_segment_ext
				WHERE logseverity = 'ERROR' AND logmessage = 'division by zero'
				  AND logdebug LIKE '%FROM lg WHERE a = 5%';")
	[[ "$out" == *"division by zero"* ]] && [ "$out2" = "$seg|true" ] \
		&& ok "a segment's error, read through __gp_log_segment_ext, names the client's statement" \
		|| notok "a segment's error in Cloudberry's log" "$out / $seg / $out2"

	# And a segment's lines of log_min_duration_statement, which the
	# segments take from the coordinator, name it: a DDL tree's here.
	printf '%s\n' "SET log_min_duration_statement = 0;" "CREATE TABLE lg2 (a int) DISTRIBUTED BY (a);" \
		"RESET log_min_duration_statement;" "SELECT count(*) FROM lg;" | qf 0 >/dev/null
	out=$(q 0 "SELECT string_agg(DISTINCT logsegment, ' ' ORDER BY logsegment)
			   FROM gp_toolkit.__gp_log_segment_ext
			   WHERE logmessage ~ '^duration: [0-9.]+ ms  statement: CREATE TABLE lg2 \\(a int\\) DISTRIBUTED BY \\(a\\);\$';
			   SELECT count(*) FROM gp_toolkit.__gp_log_segment_ext
			   WHERE logmessage LIKE 'duration: %SELECT count(*) FROM lg;';")
	[ "$out" = "seg0 seg1
0" ] && ok "log_min_duration_statement reaches the segments, whose lines name the client's statement" \
		|| notok "a segment's duration lines" "$out"

	# A message's whitespace at its end is left out of the record where
	# the client is sent the message, as Cloudberry leaves it out of both;
	# and gp_log_command_timings has the commands of this log.
	q 0 "DO \$\$ BEGIN RAISE EXCEPTION 'gp_log trailing   '; END \$\$;" >/dev/null 2>&1
	out=$(q 0 "SELECT string_agg(logmessage, '|') FROM gp_toolkit.__gp_log_coordinator_ext
			   WHERE logmessage LIKE 'gp\\_log trailing%';
			   SELECT count(*) > 0 FROM gp_toolkit.gp_log_command_timings
			   WHERE logdatabase = 'postgres' AND logsession ~ '^con' AND logduration >= '0';")
	[ "$out" = "gp_log trailing
t" ] && ok "a message's trailing whitespace off, and gp_log_command_timings" \
		|| notok "a message's trailing whitespace, and gp_log_command_timings" "$out"

	# gp.log_format = text, Cloudberry's gp_log_format, writes nothing of
	# the kind; and the views are the superuser's.
	q 0 "ALTER SYSTEM SET gp.log_format = text;" >/dev/null
	q 0 "SELECT pg_reload_conf();" >/dev/null
	sleep 1
	q 0 "SELECT 1 FROM gp_log_nowhere_3;" >/dev/null 2>&1
	q 0 "ALTER SYSTEM RESET gp.log_format;" >/dev/null
	q 0 "SELECT pg_reload_conf();" >/dev/null
	sleep 1
	q 0 "SELECT 1 FROM gp_log_nowhere_4;" >/dev/null 2>&1
	out=$(q 0 "SELECT string_agg(substring(logmessage from 'gp_log_nowhere_[0-9]'), ' ' ORDER BY logtime)
			   FROM gp_toolkit.__gp_log_coordinator_ext
			   WHERE logseverity = 'ERROR' AND logmessage LIKE '%gp\\_log\\_nowhere\\_%';
			   CREATE ROLE lg_user LOGIN;")
	out2=$("$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d postgres -U lg_user \
		-c "SELECT count(*) FROM gp_toolkit.__gp_log_master_ext;" 2>&1)
	q 0 "DROP ROLE lg_user; DROP TABLE lg, lg2;" >/dev/null
	[ "$out" = "gp_log_nowhere_1 gp_log_nowhere_2 gp_log_nowhere_4" ] &&
	[[ "$out2" == *"permission denied for view __gp_log_master_ext"* ]] \
		&& ok "gp.log_format = text writes no record, and a user who is no superuser reads none" \
		|| notok "gp.log_format, and the views' privileges" "$out / $out2"

	# gp_disk_free: each segment's space free for its data directory, as df
	# gives it, which the segment reads itself.
	out=$(q 0 "SELECT string_agg(dfsegment || ':' || (dfhostname <> '') || ':' || dfdevice || ':' || dfspace,
								 ' ' ORDER BY dfsegment)
			   FROM gp_toolkit.gp_disk_free;")
	read -r dev avail < <(df -Pk "$(datadir 1)" | awk 'NR == 2 { print $1, $4 }')
	got=$(echo "$out" | sed -n 's/^0:true:\([^:]*\):\([0-9]*\) 1:true:.*/\1 \2/p')
	isnum "${avail:-x}" && [ "${got% *}" = "$dev" ] && isnum "${got#* }" &&
	[ $(( ${got#* } - avail )) -lt 102400 ] && [ $(( avail - ${got#* } )) -lt 102400 ] \
		&& ok "gp_disk_free: each segment's device and kB free, as df -Pk says ($dev)" \
		|| notok "gp_disk_free" "$out / df: $dev $avail"

	# The checks for orphaned and missing files, which each node answers
	# of its own directories after locking its pg_class and a checkpoint: a
	# file of no relation on the coordinator and on segment 0, listed and
	# moved away as seg<id>_<path>; refused while another session is in a
	# transaction; and a table's file on a segment gone, listed missing.
	dboid=$(q 0 "SELECT oid FROM pg_database WHERE datname = 'postgres';")
	touch "$(datadir 0)/base/$dboid/999999998" "$(datadir 1)/base/$dboid/999999999"
	out=$(q 0 "SELECT string_agg(gp_segment_id || ':' || filepath, ' ' ORDER BY gp_segment_id)
			   FROM gp_toolkit.gp_check_orphaned_files WHERE filename LIKE '99999999_';")
	{ echo "BEGIN; SELECT 1;"; sleep 3; } | "$PSQL" -X -q -h "$(sockdir 0)" -p "$(port 0)" -d postgres >/dev/null 2>&1 &
	sleep 1
	out2=$(q 0 "SELECT count(*) FROM gp_toolkit.gp_check_orphaned_files;")
	wait
	mkdir -p "$ROOT/orphans"
	out3=$(q 0 "SELECT string_agg(gp_segment_id || ':' || move_success || ':' || newpath, ' ' ORDER BY gp_segment_id)
				FROM gp_toolkit.gp_move_orphaned_files('$ROOT/orphans') WHERE oldpath LIKE '%/99999999_';")
	[ "$out" = "-1:base/$dboid/999999998 0:base/$dboid/999999999" ] &&
	[[ "$out2" == *"There is a client session running on one or more segment. Aborting..."* ]] &&
	[ "$out3" = "-1:true:$ROOT/orphans/seg-1_base_${dboid}_999999998 0:true:$ROOT/orphans/seg0_base_${dboid}_999999999" ] &&
	[ -f "$ROOT/orphans/seg0_base_${dboid}_999999999" ] && [ ! -e "$(datadir 1)/base/$dboid/999999999" ] \
		&& ok "gp_check_orphaned_files and gp_move_orphaned_files, the coordinator's files and each segment's" \
		|| notok "the checks for orphaned files" "$out / $out2 / $out3"
	q 0 "CREATE TABLE mf (a int) DISTRIBUTED BY (a);
		 INSERT INTO mf SELECT generate_series(1, 10);" >/dev/null
	q 1 "CHECKPOINT;" >/dev/null
	fnode=$(q 1 "SELECT pg_relation_filenode('mf');")
	mv "$(datadir 1)/base/$dboid/$fnode" "$ROOT/mf.file"
	out=$(q 0 "SELECT string_agg(gp_segment_id || ':' || relname || ':' || (filename = '$fnode'), ' ')
			   FROM gp_toolkit.gp_check_missing_files WHERE relname = 'mf';")
	mv "$ROOT/mf.file" "$(datadir 1)/base/$dboid/$fnode"
	out2=$(q 0 "SELECT count(*) FROM gp_toolkit.gp_check_missing_files WHERE relname = 'mf';
				SELECT count(*) FROM mf; DROP TABLE mf;")
	[ "$out" = "0:mf:true" ] && [ "$out2" = "0
10" ] && ok "gp_check_missing_files lists a table's file a segment has not" \
		|| notok "gp_check_missing_files" "$out / $out2"

	# The workfile manager's (gp_workfile.c): gp_toolkit's views of the
	# temporary files, read where they lie, a row for each and each node's
	# bytes; the limits on a statement's files, in Cloudberry's words, on the
	# coordinator and in a segment's slice; and a segment's cancel, in the
	# words of Cloudberry's QE.
	out=$(q 0 "SELECT string_agg(segid || ':' || bytes, ' ' ORDER BY segid) FROM gp_toolkit.gp_workfile_mgr_used_diskspace;
			   SELECT count(*) FROM gp_toolkit.gp_workfile_entries;
			   SELECT string_agg(segid || ':' || size, ' ' ORDER BY segid) FROM gp_toolkit.gp_workfile_usage_per_segment;")
	[ "$out" = "-1:0 0:0 1:0
0
-1:0 0:0 1:0" ] && ok "gp_toolkit's workfile views: no temporary file, and 0 bytes on each node" \
		|| notok "the workfile views, nothing spilled" "$out"
	out=$(printf '%s\n' "SET work_mem = '1MB';" "BEGIN;" \
		"DECLARE wf CURSOR FOR SELECT g FROM generate_series(1, 300000) g ORDER BY g DESC;" \
		"FETCH 1 FROM wf;" \
		"SELECT count(*) || ' ' || sum(numfiles) || ' ' || bool_and(size > 0) || ' ' ||
				bool_and(sess_id = current_setting('gp.session_id')::int AND pid = pg_backend_pid() AND usename = current_user)
		 FROM gp_toolkit.gp_workfile_entries WHERE segid = -1;" \
		"SELECT size > 0 FROM gp_toolkit.gp_workfile_usage_per_query WHERE sess_id = current_setting('gp.session_id')::int;" \
		"SELECT bytes > 0 FROM gp_toolkit.gp_workfile_mgr_used_diskspace WHERE segid = -1;" \
		"COMMIT;" \
		"SELECT count(*) FROM gp_toolkit.gp_workfile_entries;" | qf 0)
	[ "$out" = "300000
2 2 true true
t
t
0" ] && ok "a cursor's spilled rows and sort on the coordinator: two files, its session's, until it ends" \
		|| notok "the workfile views of a cursor's spill" "$out"
	q 1 "SET work_mem = '1MB'; BEGIN;
		 DECLARE wf CURSOR FOR SELECT g FROM generate_series(1, 300000) g ORDER BY g DESC;
		 FETCH 1 FROM wf; SELECT pg_sleep(5); COMMIT;" >/dev/null 2>&1 &
	holder=$!
	# the cursor's two files, once its FETCH has sorted
	for i in $(seq 1 20); do
		out=$(q 0 "SELECT string_agg(segid || ':' || numfiles, ' ') FROM gp_toolkit.gp_workfile_usage_per_segment WHERE size > 0;
				   SELECT string_agg(segid::text, ' ') FROM gp_toolkit.gp_workfile_mgr_used_diskspace WHERE bytes > 0;")
		[ "$out" = "0:2
0" ] && break
		sleep 0.5
	done
	wait "$holder" 2>/dev/null
	out2=$(q 0 "SELECT sum(bytes) FROM gp_toolkit.gp_workfile_mgr_used_diskspace;")
	[ "$out|$out2" = "0:2
0|0" ] && ok "and a segment's, which its node's rows show while they last" \
		|| notok "the workfile views of a segment's spill" "$out / $out2"

	out=$(printf '%s\n' "SET work_mem = '1MB';" "SET gp.workfile_limit_per_query = '1MB';" \
		"SELECT count(DISTINCT g) FROM generate_series(1, 300000) g;" \
		"SHOW temp_file_limit;" \
		"SET temp_file_limit = '512kB';" \
		"SELECT count(DISTINCT g) FROM generate_series(1, 300000) g;" \
		"RESET temp_file_limit;" \
		"CREATE FUNCTION wf_sort(n int) RETURNS bigint LANGUAGE sql
		 AS 'SELECT count(*) FROM (SELECT g FROM generate_series(1, n) g ORDER BY g DESC) s';" \
		"SELECT wf_sort(300000) FROM gp_dist_random('gp_id');" \
		"RESET gp.workfile_limit_per_query;" \
		"SELECT wf_sort(300000) FROM gp_dist_random('gp_id');" | qf 0 |
		grep -v '^DETAIL\|^CONTEXT' | sed 's/^psql:[^:]*:[0-9]*: //' | tr '\n' '/')
	[ "$out" = 'ERROR:  workfile per query size limit exceeded/-1/ERROR:  temporary file size exceeds "temp_file_limit" (512kB)/ERROR:  workfile per query size limit exceeded/300000/300000/' ] \
		&& ok "gp.workfile_limit_per_query: a spill past it fails in Cloudberry's words, a segment's too, and a lower temp_file_limit is its own" \
		|| notok "gp.workfile_limit_per_query" "$out"
	three="SELECT count(g) FROM generate_series(1, 300000) g UNION SELECT count(g) FROM generate_series(1, 300000) g UNION SELECT count(g) FROM generate_series(1, 300000) g"
	out=$(printf '%s\n' "SET work_mem = '1MB';" "SET gp.workfile_limit_files_per_query = 2;" \
		"$three;" \
		"SET gp.workfile_limit_files_per_query = 3;" \
		"$three;" \
		"CREATE FUNCTION wf_files() RETURNS bigint LANGUAGE sql AS '$three';" \
		"SET gp.workfile_limit_files_per_query = 2;" \
		"SELECT wf_files() FROM gp_dist_random('gp_id');" | qf 0 |
		grep -v '^DETAIL\|^CONTEXT' | sed 's/^psql:[^:]*:[0-9]*: //' | tr '\n' '/')
	[ "$out" = "ERROR:  number of workfiles per query limit exceeded/300000/ERROR:  number of workfiles per query limit exceeded/" ] \
		&& ok "gp.workfile_limit_files_per_query: three spilled function scans are one file too many for 2, on the coordinator and on a segment" \
		|| notok "gp.workfile_limit_files_per_query" "$out"
	out=$(q 0 "SELECT sum(bytes) FROM gp_toolkit.gp_workfile_mgr_used_diskspace;")
	[ "$out" = "0" ] && ok "and each failed statement's files are gone with it" \
		|| notok "temporary files left by the failed statements" "$out"

	start_node 1 "gp.workfile_limit_per_segment = 1024"
	out=$(q 0 "SET work_mem = '1MB'; SELECT wf_sort(300000) FROM gp_dist_random('gp_id');" | grep -o 'ERROR:.*')
	start_node 1
	out2=$(q 0 "SET work_mem = '1MB'; SELECT wf_sort(300000) FROM gp_dist_random('gp_id'); DROP FUNCTION wf_sort(int), wf_files();" | tr '\n' ' ')
	[ "$out|$out2" = "ERROR:  workfile per segment size limit exceeded|300000 300000 " ] \
		&& ok "gp.workfile_limit_per_segment, a node's: past it there, a spill fails in Cloudberry's words" \
		|| notok "gp.workfile_limit_per_segment" "$out / $out2"

	out=$(q 0 "CREATE TABLE wfc (a int) DISTRIBUTED BY (a); INSERT INTO wfc SELECT generate_series(1, 10);
			   SELECT gp_inject_fault('exec_mpp_query_start', 'interrupt', $(dbid 1));")
	out2=$(q 0 "SELECT count(*) FROM wfc;" | grep -o 'ERROR:.*')
	q 0 "SELECT gp_inject_fault('exec_mpp_query_start', 'reset', $(dbid 1)); DROP TABLE wfc;" >/dev/null
	[ "$out|$out2" = "Success:|ERROR:  canceling MPP operation" ] \
		&& ok "a segment's cancel is Cloudberry's QE's: canceling MPP operation" \
		|| notok "a segment's cancel" "$out / $out2"

	###########################################################################
	echo "18. a segment added to the running cluster, and removed"
	###########################################################################
	# As gpexpand adds one, by hand: every table of the database given the
	# number of segments it is on (gp.expand_pin_numsegments()); node 3,
	# content 2, a copy of the coordinator -- its catalogs, and none of the
	# rows, which are the segments' -- started with its line in the file
	# every node reads; then added with gp_add_segment(), and taken by each
	# session as its next transaction begins (gp_expand.c).  And as gpshrink
	# removes it, the rows moved off it first.
	add3="SELECT gp_add_segment($(dbid 3)::int2, $(content 3)::int2, 'p', 'p', 'n', 'u', $(port 3), '$(sockdir 3)', '$(sockdir 3)', '$(datadir 3)');"
	psql0="$PSQL -X -q -t -A -h $(sockdir 0) -p $(port 0) -d postgres"
	out=$(q 0 "CREATE TABLE ex_old (a int) DISTRIBUTED BY (a); INSERT INTO ex_old SELECT generate_series(1, 30);
			   SELECT gp.expand_pin_numsegments() > 0; SELECT gp.expand_pin_numsegments();
			   SELECT label FROM pg_seclabels WHERE objoid = 'ex_old'::regclass AND provider = 'gp';" | tr '\n' ' ')
	[ "$out" = "t 0 distributed_by=(a),numsegments=2 " ] \
		&& ok "gp.expand_pin_numsegments() names the two segments in each table's label, once" \
		|| notok "gp.expand_pin_numsegments()" "$out"
	mkdir -p "$(sockdir 3)"
	if "$BINDIR/pg_basebackup" -D "$(datadir 3)" -h "$(sockdir 0)" -p "$(port 0)" \
		   -X stream -c fast > "$ROOT/basebackup3.log" 2>&1; then
		rm -f "$(datadir 3)"/gpsegconfig_dump*
		echo "$(dbid 3) $(content 3) p $(sockdir 3) $(port 3) $(datadir 3)" >> "$CONF"
		start_node 3 || notok "node 3 starts" "$(tail -5 "$ROOT/node3.log")"
	else
		notok "the coordinator copied for node 3" "$(tail -5 "$ROOT/basebackup3.log")"
	fi

	# A session that has reached the two segments with a temporary table, and
	# the segment added in another: it keeps the two while it has the table,
	# whose parts its gang's backends hold, and takes the third once DISCARD
	# TEMP has dropped it; the segments compute with three too.  A statement
	# it prepared before is planned again for three.  The table made
	# before stays on two until EXPAND TABLE, and one made after is on three.
	out=$(printf '%s\n' \
		"CREATE TEMP TABLE ex_tmp (a int) DISTRIBUTED BY (a);" \
		"SELECT count(*) FROM ex_old, gp_dist_random('gp_id') \\parse ex_ps" \
		"\\bind_named ex_ps \\g" \
		"\\! $psql0 -c \"BEGIN\" -c \"$add3\" -c \"COMMIT\" 2>&1 | grep -v '^NOTICE' | tr '\\n' ' '" \
		"SELECT count(*) FROM gp_dist_random('gp_id');" \
		"DISCARD TEMP;" \
		"\\bind_named ex_ps \\g" \
		"SELECT string_agg(n.content_id || ':' || n.segments, ',' ORDER BY n.content_id) FROM (SELECT (gp.node()).* FROM gp_dist_random('gp_id')) n;" \
		"SELECT count(DISTINCT gp_segment_id) || ' ' || count(*) FROM ex_old;" \
		"ALTER TABLE ex_old EXPAND TABLE;" \
		"SELECT count(DISTINCT gp_segment_id) || ' ' || count(*) FROM ex_old;" \
		"CREATE TABLE ex_new (a int) DISTRIBUTED BY (a);" \
		"INSERT INTO ex_new SELECT generate_series(1, 30);" \
		"SELECT count(DISTINCT gp_segment_id) || ' ' || count(*) FROM ex_new;" \
		"SELECT string_agg(numsegments::text, ' ' ORDER BY localoid) FROM gp_distribution_policy WHERE localoid IN ('ex_old'::regclass, 'ex_new'::regclass);" | qf 0 | tr '\n' '/')
	[ "$out" = "60/4 2/90/0:3,1:3,2:3/2 30/3 30/3 30/3 3/" ] \
		&& ok "a session takes the segment added as its next transaction begins, once it has no temporary table: an old table stays on two until EXPAND TABLE" \
		|| notok "a segment added while a session is idle" "$out"
	out=$(q 0 "SELECT segments FROM gp.node(); SELECT count(*) FROM gp_segment_configuration WHERE content = $(content 3) AND role = 'p' AND status = 'u';" | tr '\n' ' ')
	[ "$out" = "3 1 " ] && ok "a new session has three segments, the new one up in gp_segment_configuration" \
		|| notok "a new session after the segment was added" "$out"

	# A session that kept the old segments may change no catalog, as
	# Cloudberry's may not: gpexpand's version bumped under a session with a
	# temporary table ends it at its next DDL.
	out=$(printf '%s\n' \
		"CREATE TEMP TABLE ex_tmp (a int) DISTRIBUTED BY (a);" \
		"\\! $psql0 -c \"SELECT gp_expand_bump_version()\"" \
		"CREATE TABLE ex_fatal (a int);" | qf 0 | grep -o 'FATAL:.*')
	[ "$out" = "FATAL:  cluster is expanded from version 1 to 2, catalog changes are disallowed" ] \
		&& ok "a session that kept the old segments is ended at its next catalog change" \
		|| notok "a catalog change in a session that kept the old segments" "$out"

	out=$(q 0 "BEGIN; SELECT gp_add_segment(9::int2, 4::int2, 'p', 'p', 'n', 'u', $(port 3), '$(sockdir 3)', '$(sockdir 3)', '$ROOT/nowhere'); COMMIT;" 2>&1 | grep -o 'ERROR:.*')
	out2=$(q 0 "BEGIN; SELECT gp_add_segment(9::int2, 64::int2, 'p', 'p', 'n', 'u', $(port 3), '$(sockdir 3)', '$(sockdir 3)', '$ROOT/nowhere'); COMMIT;" 2>&1 | grep -o 'ERROR:.*')
	out3=$(q 0 "SELECT gp_remove_segment($(dbid 1)::int2);" 2>&1 | grep -o 'ERROR:.*')
	[ "$out|$out2|$out3" = "ERROR:  content 3 would have no node|ERROR:  the cluster has no room for content 64|ERROR:  content 0 would have no node" ] \
		&& ok "a segment is added after the last, within gp.max_segments, and only the last is removed" \
		|| notok "the checks of a segment added or removed" "$out / $out2 / $out3"

	# gpshrink's way back: the rows moved to the first two, then the segment
	# removed and stopped; the session that had three takes two.
	out=$(printf '%s\n' \
		"ALTER TABLE ex_old SHRINK TABLE TO 2;" \
		"ALTER TABLE ex_new SHRINK TABLE TO 2;" \
		"SELECT count(DISTINCT gp_segment_id) || ' ' || count(*) FROM ex_new;" \
		"\\! $psql0 -c \"SELECT gp_remove_segment($(dbid 3)::int2)\" 2>&1 | tr '\\n' ' '" \
		"SELECT segments FROM gp.node();" \
		"SELECT count(*) FROM ex_new;" | qf 0 | tr '\n' '/')
	"$BINDIR/pg_ctl" -D "$(datadir 3)" -m fast stop > /dev/null 2>&1
	[ "$out" = "2 30/t 2/30/" ] \
		&& ok "the rows moved off it, the segment removed: a session with a gang to three takes two" \
		|| notok "a segment removed" "$out"
	out=$(q 0 "SELECT segments FROM gp.node(); SELECT count(*) FROM gp_segment_configuration;
			   SELECT count(DISTINCT gp_segment_id) || ' ' || count(*) FROM ex_old; SELECT count(*) FROM ex_new;
			   DROP TABLE ex_old, ex_new;" | tr '\n' ' ')
	out2=$(grep -c "^$(dbid 3) " "$CONF")
	[ "$out|$out2" = "2 3 2 30 30 |0" ] && ok "and every session has two again, the tables their rows, the file no line of it" \
		|| notok "the cluster after the segment was removed" "$out / $out2"

	###########################################################################
	echo "19. parallel retrieve cursors, without ORCA: the coordinator's endpoint"
	###########################################################################
	# Without gp_orca every parallel retrieve cursor's endpoint is the
	# coordinator's, filled as DECLARE runs the planner's plan (gp_endpoint.c);
	# a retrieve session there reads it, logged in with the cursor's token as
	# the port's setting or as Cloudberry's password.  The isolation2 suite
	# runs Cloudberry's tests of them, whose endpoints ORCA puts on the
	# segments.
	q 0 "CREATE TABLE prc (a int) DISTRIBUTED BY (a); INSERT INTO prc SELECT generate_series(1, 10);" >/dev/null
	retrieve() {				# retrieve <options> <password>, $PRC_NAME's rows
		echo "\\! PGOPTIONS=\"$1\" PGPASSWORD=\"$2\" $PSQL -X -q -t -A -h $(sockdir 0) -p $(port 0) -d postgres -c \"RETRIEVE ALL FROM ENDPOINT \$PRC_NAME\" 2>&1 | sort -n | tr '\\n' ' '; echo"
	}
	out=$({
		echo "BEGIN;"
		echo "DECLARE c PARALLEL RETRIEVE CURSOR FOR SELECT * FROM prc;"
		echo "SELECT gp_segment_id, state FROM gp_get_endpoints() WHERE cursorname = 'c';"
		echo "SELECT auth_token AS token, endpointname AS name FROM gp_get_endpoints() WHERE cursorname = 'c' \\gset"
		echo "\\setenv PRC_TOKEN :token"
		echo "\\setenv PRC_NAME :name"
		retrieve "-c gp.retrieve_token=\$PRC_TOKEN" ""
		echo "SELECT * FROM gp_wait_parallel_retrieve_cursor('c', 0);"
		echo "ROLLBACK;"
	} | qf 0 | tr '\n' '/')
	[ "$out" = "-1|READY/1 2 3 4 5 6 7 8 9 10 /t/" ] \
		&& ok "an endpoint on the coordinator, read by a retrieve session with gp.retrieve_token" \
		|| notok "a coordinator's endpoint, read with gp.retrieve_token" "$out"
	out=$({
		echo "BEGIN;"
		echo "DECLARE c PARALLEL RETRIEVE CURSOR FOR SELECT a * 2 FROM prc ORDER BY 1 DESC;"
		echo "SELECT auth_token AS token, endpointname AS name FROM gp_get_endpoints() WHERE cursorname = 'c' \\gset"
		echo "\\setenv PRC_TOKEN :token"
		echo "\\setenv PRC_NAME :name"
		retrieve "-c gp_retrieve_conn=true" "0123456789abcdef0123456789abcdef"
		retrieve "-c gp_retrieve_conn=true" "not a token"
		retrieve "-c gp_retrieve_conn=true" "\$PRC_TOKEN"
		echo "SELECT * FROM gp_wait_parallel_retrieve_cursor('c', 0);"
		echo "ROLLBACK;"
	} | qf 0 | sed 's/.*FATAL:  //' | tr '\n' '/')
	[ "$out" = "Authentication failure (Wrong password or no endpoint for the user) /retrieve auth token is invalid /2 4 6 8 10 12 14 16 18 20 /t/" ] \
		&& ok "and with Cloudberry's gp_retrieve_conn and the token as a password, a wrong one refused" \
		|| notok "a coordinator's endpoint, read with gp_retrieve_conn" "$out"
	out=$(q 0 "SELECT count(*) FROM gp_get_endpoints(); DROP TABLE prc;")
	[ "$out" = "0" ] && ok "and the cursors' ends leave no endpoint" \
		|| notok "endpoints left" "$out"
fi

###############################################################################
echo "20. a cluster described wrongly is a server that does not start"
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
echo "21. with no cluster configured, this is a single node"
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

	out=$(q 0 "SELECT dbid, content, role, mode, status, port, datadir = current_setting('data_directory') FROM gp_segment_configuration;")
	[ "$out" = "1|-1|p|n|u|$(port 0)|t" ] && ok "gp_segment_configuration lists the one node, as Cloudberry's single node does" \
		|| notok "gp_segment_configuration with no cluster" "$out"

	out=$(q 0 "SELECT count(*) FROM gp_distribution_policy;")
	[ "$out" = "0" ] && ok "and gp_distribution_policy none, whatever the labels say" \
		|| notok "gp_distribution_policy with no cluster" "$out"

	out=$(q 0 "CREATE TABLE sn (a int); INSERT INTO sn VALUES (1), (2); SELECT DISTINCT gp_segment_id FROM sn;")
	[ "$out" = "-1" ] && ok "gp_segment_id is -1, as on Cloudberry's single node" \
		|| notok "gp_segment_id on one node" "$out"

	out=$(q 0 "BEGIN ISOLATION LEVEL SERIALIZABLE; SHOW transaction_isolation; COMMIT;")
	[ "$out" = "serializable" ] && ok "and SERIALIZABLE is PostgreSQL's, which is serializable on one node" \
		|| notok "SERIALIZABLE on one node" "$out"
else
	notok "a server with no cluster starts" "$(tail -5 "$ROOT/node0.log")"
fi

###############################################################################
echo "22. parallelism within a segment"
###############################################################################
# gp_parallel.c: with gp.enable_parallel on, a segment's writer runs a Gather
# of PostgreSQL's in what it runs for the coordinator -- a gather's query of
# the planner's route that the coordinator reads to its end, planned there
# with parallel plans allowed, or a fragment of ORCA's that the ORCA module
# gave Gathers (orca/parallel.c) -- whole at its first FETCH; and a reader,
# a member of the writer's lock group, starts none.  The evidence is each
# segment's own count of the workers it launched (pg_stat_database), which a
# session's segment backends report as they end with it.
if [ "$started" -eq 1 ]; then
	par_started=1
	for n in 1 2 0; do
		start_node "$n" "shared_preload_libraries = '$PRELOAD,gp_orca,gp_ao,pax'" \
			"gp.cluster_secret = '$SECRET'" "max_worker_processes = 16" \
			"max_parallel_workers = 8" || par_started=0
	done
	out=$(q 0 "SET client_min_messages = warning; CREATE EXTENSION IF NOT EXISTS gp_orca;
			   CREATE EXTENSION gp_ao; CREATE EXTENSION pax;
			   CREATE TABLE ph (a int, b int) WITH (parallel_workers = 2) DISTRIBUTED BY (a);
			   CREATE TABLE pa (a int, b int) USING ao_row WITH (parallel_workers = 2) DISTRIBUTED BY (a);
			   CREATE TABLE pc (a int, b int) USING ao_column WITH (parallel_workers = 2) DISTRIBUTED BY (a);
			   CREATE TABLE pp (a int, b int) USING pax WITH (parallel_workers = 2) DISTRIBUTED BY (a);
			   SET gp.appendonly_insert_files = 4;
			   INSERT INTO ph SELECT i, i FROM generate_series(1, 300000) i;
			   INSERT INTO pa SELECT i, i FROM generate_series(1, 300000) i;
			   INSERT INTO pc SELECT i, i FROM generate_series(1, 300000) i;
			   INSERT INTO pp SELECT i, i FROM generate_series(1, 300000) i;
			   ANALYZE ph; ANALYZE pa; ANALYZE pc; ANALYZE pp;")
	if [ "$par_started" -eq 1 ] && [ -z "$out" ]; then
		ok "the cluster restarts with room for workers, and a heap, an AO row, an AO column and a PAX table of 300,000 rows"
	else
		notok "the cluster for parallelism within a segment" "$out $(tail -3 "$ROOT/node0.log")"
	fi

	# what makes a Gather pay on tables this small, as a session sets it
	PAR="SET gp.enable_parallel = on; SET max_parallel_workers_per_gather = 2;
		 SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
		 SET min_parallel_table_scan_size = 0;"
	par_launched() {			# each segment's count of the workers it launched
		echo "$(q 1 "SELECT parallel_workers_launched FROM pg_stat_database WHERE datname = current_database();")" \
			"$(q 2 "SELECT parallel_workers_launched FROM pg_stat_database WHERE datname = current_database();")"
	}
	par_after() {				# par_after <before>: once both have gone past it, or 5 s on
		local now i
		for i in $(seq 1 10); do
			now=$(par_launched)
			par_more "$1" "$now" && break
			sleep 0.5
		done
		echo "$now"
	}
	par_more() {				# par_more <before> <after>: both segments launched more
		local b1 b2 a1 a2
		read -r b1 b2 <<< "$1"; read -r a1 a2 <<< "$2"
		isnum "$b1" && isnum "$b2" && isnum "$a1" && isnum "$a2" && [ "$a1" -gt "$b1" ] && [ "$a2" -gt "$b2" ]
	}
	# par_same <what> <optimizer> <sql> [EXPLAIN's pattern]: the rows with
	# workers are the rows without, and both segments launched workers
	par_same() {
		local want got before after plan
		want=$(q 0 "SET gp.optimizer = $2; $3")
		before=$(par_launched)
		got=$(q 0 "SET gp.optimizer = $2; $PAR $3")
		after=$(par_after "$before")
		plan=$(q 0 "SET gp.optimizer = $2; $PAR EXPLAIN (COSTS OFF) $3")
		if [ -z "$want" ] || [ "$got" != "$want" ]; then
			notok "$1" "want [$want] got [$got]"
		elif ! par_more "$before" "$after"; then
			notok "$1: workers on each segment" "launched $before, then $after"
		elif [ -n "${4:-}" ] && [[ "$plan" != $4 ]]; then
			notok "$1: the plan" "$plan"
		else
			ok "$1"
		fi
	}

	for t in ph pa pc pp; do
		par_same "the planner's route: a gather of $t read whole on each segment, by the writer and its workers" \
			off "SELECT count(*), sum(b) FROM $t WHERE b % 7 = 0;"
	done
	for t in ph pa pc pp; do
		par_same "ORCA: a Gather over the scan of $t in the writer's slice, below the Gather Motion" \
			on "SELECT count(*), sum(b) FROM $t WHERE b % 7 = 0;" \
			"*Gather Motion 2:1  (slice1; segments: 2)*Gather*Workers Planned: 2*Parallel Seq Scan on $t*"
	done
	par_same "ORCA: the rows of a scan in the writer's slice, through a Gather" \
		on "SELECT a, b FROM ph WHERE b % 7 = 0 ORDER BY a;" \
		"*Gather Motion*Gather*Workers Planned: 2*Parallel Seq Scan on ph*"
	par_same "ORCA: an aggregate in three stages, each participant's, the segment's and the coordinator's, numeric states serialized between them" \
		on "SELECT count(*), sum(b::numeric), avg(b::numeric), max(b) FROM ph WHERE b % 7 = 0;" \
		"*Finalize Aggregate*Gather Motion*Partial Aggregate*Gather*Workers Planned: 2*Partial Aggregate*Parallel Seq Scan on ph*"
	par_same "ORCA: a join on the distribution key split among the participants, the outer side's scan shared, the inner side's hashed by each" \
		on "SELECT count(*) FROM ph JOIN pa USING (a) WHERE ph.b % 7 = 0;" \
		"*Gather*Workers Planned: 2*Partial Aggregate*Hash Join*Parallel Seq Scan on pa*Hash*Seq Scan on ph*"

	out=$(q 0 "$PAR EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM ph WHERE b % 7 = 0;" |
		grep -c "Workers Launched: 2")
	[ "$out" = "1" ] && ok "EXPLAIN ANALYZE says what the segments' Gather launched: Workers Launched: 2" \
		|| notok "EXPLAIN ANALYZE's Workers Launched" "$out"

	# A reader's slice: the scan below a Redistribute is a reader's, which
	# is a member of the writer's lock group and starts no workers.
	want=$(q 0 "SET gp.optimizer = off; SELECT b % 10, count(*) FROM ph GROUP BY 1 ORDER BY 1;")
	sleep 1
	before=$(par_launched)
	got=$(q 0 "$PAR SELECT b % 10, count(*) FROM ph GROUP BY 1 ORDER BY 1;")
	plan=$(q 0 "$PAR EXPLAIN (COSTS OFF) SELECT b % 10, count(*) FROM ph GROUP BY 1 ORDER BY 1;")
	sleep 1
	after=$(par_launched)
	if [ -n "$want" ] && [ "$got" = "$want" ] && [ "$after" = "$before" ] &&
		[[ "$plan" == *"Redistribute Motion 2:2"* ]] && [[ "$plan" != *"Workers Planned"* ]]; then
		ok "ORCA: a scan in a reader's slice, below a Redistribute, has no Gather and starts no workers"
	else
		notok "a reader's slice without workers" "[$got] [$want] $before -> $after $plan"
	fi

	# Not read whole, not run whole: a gather below a LIMIT the segments
	# are not sent, and a cursor's -- in batches, as before, with no workers.
	sleep 1
	before=$(par_launched)
	out=$(q 0 "SET gp.optimizer = off; $PAR SELECT count(*) FROM (SELECT a FROM ph WHERE b % 7 = 0 AND a > random() - 2 LIMIT 5) s;")
	out2=$(printf '%s\n' "SET gp.optimizer = off;" "$PAR" "BEGIN;" \
		"DECLARE pc1 CURSOR FOR SELECT a FROM ph WHERE b % 7 = 0;" \
		"FETCH 2 FROM pc1;" "COMMIT;" | qf 0 | wc -l)
	sleep 1
	after=$(par_launched)
	[ "$out|$out2|$after" = "5|2|$before" ] \
		&& ok "the planner's route: a gather below a LIMIT not sent, and a cursor's, are fetched in batches, with no workers" \
		|| notok "gathers not read whole" "$out / $out2 / $before -> $after"

	# A plan made with workers, carried out without them: a prepared
	# statement's cached plan, gp.enable_parallel turned off since -- the
	# writer takes its fragment's Gathers out, and launches none.
	want=$(q 0 "SELECT count(*), sum(b) FROM ph WHERE b % 7 = 0;")
	out=$(printf '%s\n' "$PAR" "PREPARE pps AS SELECT count(*), sum(b) FROM ph WHERE b % 7 = 0;" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) EXECUTE pps;" \
		"SET gp.enable_parallel = off;" \
		"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) EXECUTE pps;" \
		"EXECUTE pps;" | qf 0 | grep -E "Workers Launched|^[0-9]" | tr -s ' ' | tr '\n' '/')
	[ -n "$want" ] && [ "$out" = " Workers Launched: 2/ Workers Launched: 0/$want/" ] \
		&& ok "ORCA: a cached plan with Gathers, gp.enable_parallel off since, runs without them" \
		|| notok "a cached plan's Gathers" "$want / $out"

	# The transaction's own work, seen by the workers: its rows written
	# earlier, and a combo command id of its own rows it updated -- heap,
	# AO and PAX, PAX's deletes too -- and gp.dtx_xid reported by the
	# writer, not its workers, which could set no setting.
	for opt in off on; do
		before=$(par_launched)
		out=$(printf '%s\n' "SET gp.optimizer = $opt;" "$PAR" "BEGIN;" \
			"INSERT INTO ph SELECT i, 7 FROM generate_series(300001, 300100) i;" \
			"SELECT count(*) FROM ph WHERE b % 7 = 0;" \
			"UPDATE ph SET b = b WHERE a > 300000;" \
			"SELECT count(*) FROM ph WHERE b % 7 = 0;" \
			"INSERT INTO pa SELECT i, 7 FROM generate_series(300001, 300100) i;" \
			"SELECT count(*) FROM pa WHERE b % 7 = 0;" \
			"INSERT INTO pp SELECT i, 7 FROM generate_series(300001, 300100) i;" \
			"SELECT count(*) FROM pp WHERE b % 7 = 0;" \
			"DELETE FROM pp WHERE a <= 1000;" \
			"SELECT count(*) FROM pp WHERE b % 7 = 0;" \
			"ROLLBACK;" | qf 0 | tr '\n' ' ')
		after=$(par_after "$before")
		if [ "$out" = "42957 42957 42957 42957 42815 " ] && par_more "$before" "$after"; then
			ok "gp.optimizer = $opt: workers see what their writer's transaction wrote, updated and deleted"
		else
			notok "the writer's own work under workers, gp.optimizer = $opt" "$out / $before -> $after"
		fi
	done

	# The distributed snapshot, as section 13 checks it, under workers: the
	# snapshot the writer made to agree with it is its workers', a commit it
	# hides hidden from them, and what it deleted kept for them by VACUUM.
	# The sessions are kept in step by files their psql waits for (\!): the
	# reading one takes its distributed snapshot, the others do their work,
	# and only then does it read -- the segments' own snapshots taken after.
	par_step() {				# par_step <name>: the psql line that waits for it
		echo "\\! while [ ! -f $ROOT/par_$1 ]; do sleep 0.1; done"
	}
	par_signal() {				# par_signal <name>: the psql line that gives it
		echo "\\! touch $ROOT/par_$1"
	}
	par_await() {				# par_await <name>: the shell waits for it, 30 s at most
		local i
		for i in $(seq 1 300); do [ -f "$ROOT/par_$1" ] && return 0; sleep 0.1; done
		return 1
	}
	# par_read <opt> <extra settings>: a REPEATABLE READ reader in the
	# background, its snapshot taken, waiting for "go"; its answer in par_read.out
	par_read() {
		rm -f "$ROOT"/par_ready "$ROOT"/par_go
		printf '%s\n' "SET gp.optimizer = $1;" "$PAR" "$2" "BEGIN ISOLATION LEVEL REPEATABLE READ;" \
			"SELECT 'established';" "$(par_signal ready)" "$(par_step go)" \
			"SELECT count(*), sum(b) FROM pd WHERE b % 7 = 0;" "COMMIT;" |
			qf 0 > "$ROOT/par_read.out" 2>&1 &
		par_reader=$!
		par_await ready
	}
	# ANALYZE before each, for ORCA's costs: a VACUUM of a distributed table
	# while a snapshot still sees what it deleted leaves the coordinator's
	# reltuples 0.
	q 0 "CREATE TABLE pd (a int, b int) WITH (parallel_workers = 2) DISTRIBUTED BY (a);
		 INSERT INTO pd SELECT i, i FROM generate_series(1, 100000) i;" >/dev/null
	for opt in off on; do
		q 0 "ANALYZE pd;" >/dev/null
		want=$(q 0 "SELECT count(*), sum(b) FROM pd WHERE b % 7 = 0;")
		before=$(par_launched)
		par_read "$opt" ""
		q 0 "INSERT INTO pd SELECT i, 7 FROM generate_series(100001, 100010) i;" >/dev/null
		touch "$ROOT/par_go"
		wait "$par_reader"
		out=$(tail -1 "$ROOT/par_read.out")
		after=$(par_after "$before")
		if [ -n "$want" ] && [ "$out" = "$want" ] && par_more "$before" "$after"; then
			ok "gp.optimizer = $opt: a snapshot taken before a commit hides it from the segments' workers too"
		else
			notok "a distributed snapshot's view under workers, gp.optimizer = $opt" "$want / $out / $before -> $after"
		fi

		want=$(q 0 "SELECT count(*), sum(b) FROM pd WHERE b % 7 = 0;")
		before=$(par_launched)
		par_read "$opt" ""
		q 0 "DELETE FROM pd WHERE a > 100000;" >/dev/null
		q 0 "VACUUM pd;" >/dev/null
		q 0 "ANALYZE pd;" >/dev/null
		touch "$ROOT/par_go"
		wait "$par_reader"
		out=$(tail -1 "$ROOT/par_read.out")
		after=$(par_after "$before")
		if [ -n "$want" ] && [ "$out" = "$want" ] && par_more "$before" "$after"; then
			ok "gp.optimizer = $opt: VACUUM keeps what a hidden transaction deleted for the workers too"
		else
			notok "the horizon under workers, gp.optimizer = $opt" "$want / $out / $before -> $after"
		fi
	done

	# A worker's xmin, under REPEATABLE READ: the made snapshot's, lower
	# than the writer's own transaction snapshot's, which PostgreSQL gives
	# its workers.  A snapshot of a segment where another transaction has
	# more subtransactions than a backend keeps (suboverflowed) finds a
	# hidden transaction's subtransaction in pg_subtrans, which asserts that
	# it is no older than the backend's xmin.  The hidden one commits after
	# the reader's distributed snapshot, having written in a subtransaction;
	# the overflowing one begins after it, and holds 100 subtransactions,
	# each writing on both segments, open while the workers -- the workers
	# alone, the writer not taking part -- read the table; and a write after
	# it has committed, so that the reader's snapshot has it in progress
	# rather than beyond its xmax.  Without the xmin, an assert-enabled
	# server's worker fails in SubTransGetTopmostTransaction().
	for opt in off on; do
		q 0 "ANALYZE pd;" >/dev/null
		want=$(q 0 "SELECT count(*), sum(b) FROM pd WHERE b % 7 = 0;")
		before=$(par_launched)
		par_read "$opt" "SET parallel_leader_participation = off;"
		printf '%s\n' "BEGIN;" "SAVEPOINT s;" \
			"INSERT INTO pd SELECT i, 7 FROM generate_series(200001, 200010) i;" \
			"RELEASE s;" "COMMIT;" | qf 0 > /dev/null 2>&1
		rm -f "$ROOT"/par_open "$ROOT"/par_end
		{
			echo "BEGIN;"
			for i in $(seq 1 100); do
				echo "SAVEPOINT s$i; INSERT INTO pd SELECT g, 1 FROM generate_series($((300000 + 10 * i)), $((300007 + 10 * i))) g;"
			done
			par_signal open
			par_step end
			echo "ROLLBACK;"
		} | qf 0 > /dev/null 2>&1 &
		overflow=$!
		par_await open
		q 0 "INSERT INTO pd SELECT i, 1 FROM generate_series(400001, 400004) i;" >/dev/null
		touch "$ROOT/par_go"
		wait "$par_reader"
		touch "$ROOT/par_end"
		wait "$overflow"
		out=$(tail -1 "$ROOT/par_read.out")
		after=$(par_after "$before")
		if [ -n "$want" ] && [ "$out" = "$want" ] && par_more "$before" "$after"; then
			ok "gp.optimizer = $opt: a worker finds a hidden subtransaction under REPEATABLE READ, its xmin the made snapshot's"
		else
			notok "a worker's xmin under REPEATABLE READ, gp.optimizer = $opt" "$want / $out / $before -> $after"
		fi
		q 0 "DELETE FROM pd WHERE a > 100000;" >/dev/null
	done
	q 0 "DROP TABLE ph, pa, pc, pp, pd;" >/dev/null
fi

###############################################################################
echo "23. a runtime filter's range and PAX's sparse filter"
###############################################################################
# gp_rtfilter.c: a runtime filter's range reaches a PAX table's scan as its
# keys, and PAX's sparse filter skips the files whose statistics rule the
# range out, as Cloudberry's PAX, which says SCAN_SUPPORT_RUNTIME_FILTER, is
# given it -- under ORCA the segments' scans, begun with the keys, and on
# the planner's route through the SQL the gather sends.  Ten files on each
# segment, each of a thousand values of its own, and a join on ten values;
# PAX says what it skipped in its log (pax.enable_debug).
if [ "${par_started:-0}" -eq 1 ]; then
	q 0 "CREATE TABLE rfp (c1 int, c2 int) USING pax WITH (minmax_columns = 'c2') DISTRIBUTED BY (c1);
	     CREATE TABLE rfq (c2 int) DISTRIBUTED REPLICATED;
	     INSERT INTO rfq SELECT generate_series(5000, 5009);" >/dev/null
	for k in $(seq 0 9); do
		q 0 "INSERT INTO rfp SELECT i, i FROM generate_series($((k * 1000)), $((k * 1000 + 999))) i;" >/dev/null
	done
	q 0 "ANALYZE rfp; ANALYZE rfq;" >/dev/null
	rf_pax="SELECT count(*) FROM rfp JOIN rfq USING (c2);"
	rf_explain="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)"
	sp=$(printf '%26s' '')
	out=$(for sparse in on off; do
		printf '%s\n' "SET gp.enable_runtime_filter_pushdown = on;" "SET pax.enable_sparse_filter = $sparse;" \
			"$rf_explain $rf_pax" "$rf_pax" | qf 0
	done | tr '\n' '|')
	case "$out" in
		*"->  Seq Scan on rfp (actual rows=7.00 loops=1)|${sp}Rows Removed by Pushdown Runtime Filter: 521|"*"|10|"*"->  Seq Scan on rfp (actual rows=7.00 loops=1)|${sp}Rows Removed by Pushdown Runtime Filter: 5006|"*"|10|")
			ok "under ORCA a segment's scan of a PAX table, begun with the range, reads the one file in it: 521 rows dropped where the ten files' are 5,006" ;;
		*) notok "a PAX scan begun with a runtime filter's range" "$out" ;;
	esac
	logged() { cat "$ROOT/node1.log" "$ROOT/node2.log" | grep -c "kind file, filter rate: 9 / 10"; }
	before=$(logged)
	out=$(printf '%s\n' "SET gp.optimizer = off;" "SET gp.enable_runtime_filter_pushdown = on;" \
		"SET pax.enable_debug = on;" "$rf_explain $rf_pax" "$rf_pax" | qf 0 | tr '\n' '|')
	after=$(logged)
	case "$out" in
		*"->  Gather Motion 2:1 on rfp  (slice1; segments: 2) (actual rows=10.00 loops=1)|"*"Rows Removed by Pushdown Runtime Filter: 0|"*"|10|")
			if [ "$((after - before))" -ge 2 ]; then
				ok "on the planner's route the segments are sent the range, and each one's PAX scan skips nine of its ten files"
			else
				notok "PAX's sparse filter by a gather's range" "the segments logged $before, then $after, files skipped"
			fi ;;
		*) notok "a gather's range on a PAX table" "$out" ;;
	esac
	# A segment's own plan: a hash join made again for each row of a
	# subquery's, its scan of a PAX table begun again with each new range --
	# one file read for each, 995 of its rows dropped -- and every answer the
	# one without the filter, a LIMIT's that stops the scan short among them.
	q 1 "CREATE TABLE rfsp (c1 int, c2 int) USING pax WITH (minmax_columns = 'c2');
	     CREATE TABLE rfsq (c2 int);
	     INSERT INTO rfsq SELECT generate_series(100, 109) UNION ALL SELECT generate_series(5000, 5009)
	         UNION ALL SELECT generate_series(7000, 7009);" >/dev/null
	for k in $(seq 0 9); do
		q 1 "INSERT INTO rfsp SELECT i, i FROM generate_series($((k * 1000)), $((k * 1000 + 999))) i;" >/dev/null
	done
	q 1 "ANALYZE rfsp; ANALYZE rfsq;" >/dev/null
	rf_each="SELECT x.lo, (SELECT count(*) FROM rfsp JOIN rfsq ON rfsp.c2 = rfsq.c2 WHERE rfsq.c2 BETWEEN x.lo AND x.lo + 4) FROM (VALUES (5000), (7000), (100), (5000), (9999), (7005)) x(lo) ORDER BY 1, 2;"
	rf_short="SELECT x.lo, (SELECT count(*) FROM (SELECT 1 FROM rfsp JOIN rfsq ON rfsp.c2 = rfsq.c2 WHERE rfsq.c2 BETWEEN x.lo AND x.lo + 4 LIMIT 2) s) FROM (VALUES (5000), (7000), (100), (5000)) x(lo) ORDER BY 1, 2;"
	out=$(for on in on off; do
		printf '%s\n' "SET enable_nestloop = off;" "SET enable_mergejoin = off;" \
			"SET gp.enable_runtime_filter_pushdown = $on;" "$rf_each" "$rf_short" | qf 1
	done | tr '\n' ' ')
	plan=$(printf '%s\n' "SET enable_nestloop = off;" "SET enable_mergejoin = off;" \
		"SET gp.enable_runtime_filter_pushdown = on;" "$rf_explain $rf_each" | qf 1 | tr '\n' '|')
	want="100|5 5000|5 5000|5 7000|5 7005|5 9999|0 100|2 5000|2 5000|2 7000|2 "
	case "$out|$plan" in
		"$want$want|"*"->  Seq Scan on rfsp (actual rows=5.00 loops=5)|"*"Rows Removed by Pushdown Runtime Filter: 4975|"*)
			ok "a segment's PAX scan under a hash join made again for each row is begun again with each range, and answers as without" ;;
		*) notok "a PAX scan's range at each rescan" "$out / $plan" ;;
	esac
	q 0 "DROP TABLE rfp, rfq;" >/dev/null
	q 1 "DROP TABLE rfsp, rfsq;" >/dev/null
fi


###############################################################################
echo "24. pg_hint_plan's hint table, on the cluster and on one node"
###############################################################################
# pg_hint_plan's hints from its table, hint_plan.hints, found by a
# statement's text with its constants made '?' and by the application's
# name, under pg_hint_plan.enable_hint_table: the table is replicated, as a
# table an extension's script makes is, and read through a gather.  ORCA is
# given the hint state the module's planner hook made of the query, where
# Cloudberry's plan_hint_hook finds the hints again -- a lookup of the table,
# which ORCA, planning one query, cannot plan too, and which failed every
# statement ORCA planned while the setting was on (pg_hint_plan.c).
if [ "$started" -eq 1 ]; then
	# gp_ao and pax as well, whose records the nodes' WAL has (22.)
	hint_started=1
	for n in 1 2 0; do
		start_node "$n" "shared_preload_libraries = '$PRELOAD,gp_orca,gp_ao,pax,pg_hint_plan'" \
			"gp.cluster_secret = '$SECRET'" || hint_started=0
	done
	out=$(printf '%s\n' "SET client_min_messages = warning;" \
		"CREATE EXTENSION IF NOT EXISTS gp_orca;" "CREATE EXTENSION pg_hint_plan;" \
		"CREATE TABLE ht (id int PRIMARY KEY, val int) DISTRIBUTED BY (id);" \
		"INSERT INTO ht SELECT i, i % 100 FROM generate_series(1, 10000) i;" "ANALYZE ht;" \
		"INSERT INTO hint_plan.hints (norm_query_string, application_name, hints) VALUES
			('EXPLAIN (COSTS false) SELECT count(*) FROM ht a JOIN ht b ON a.id = b.val WHERE a.id < ?;', '', 'MergeJoin(a b)'),
			('EXPLAIN (COSTS false) SELECT count(*) FROM ht a JOIN ht b ON a.id = b.val WHERE a.id < ?;', 'hintapp', 'NestLoop(a b)'),
			('EXPLAIN (COSTS false) SELECT * FROM ht WHERE ht.id = ?;', '', 'SeqScan(ht)');" \
		"SELECT policytype FROM gp_distribution_policy WHERE localoid = 'hint_plan.hints'::regclass;" \
		"SELECT count(*) FROM gp_dist_random('hint_plan.hints');" | qf 0 | tr '\n' ' ')
	[ "$hint_started" -eq 1 ] && [ "$out" = "r 6 " ] \
		&& ok "pg_hint_plan's hint table is replicated, as a table an extension's script makes is, its rows on each segment" \
		|| notok "pg_hint_plan's hint table on the cluster" "$out"

	# the join the coordinator makes of two gathers, under the planner
	hint_join() {
		printf '%s\n' "SET gp.optimizer = off;" "SET pg_hint_plan.enable_hint_table = on;" "$@" \
			"EXPLAIN (COSTS false) SELECT count(*) FROM ht a JOIN ht b ON a.id = b.val WHERE a.id < 10;" |
			qf 0 | grep -o 'Hash Join\|Merge Join\|Nested Loop' | head -1
	}
	out="$(hint_join)|$(hint_join "SET application_name = 'hintapp';")|$(hint_join "SET pg_hint_plan.enable_hint_table = off;")"
	[ "$out" = "Merge Join|Nested Loop|Hash Join" ] \
		&& ok "under the planner the table's hint joins the coordinator's gathers, the application's own row before the one of any; none with the setting off" \
		|| notok "the hint table under the planner" "$out"

	errors=$(q 0 "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'error';")
	out=$(printf '%s\n' "SET gp.optimizer = on;" "SET pg_hint_plan.enable_hint_table = on;" \
		"EXPLAIN (COSTS false) SELECT * FROM ht WHERE ht.id = 1;" \
		"SET pg_hint_plan.enable_hint_table = off;" \
		"EXPLAIN (COSTS false) SELECT * FROM ht WHERE ht.id = 1;" | qf 0 |
		grep -o 'Seq Scan on ht\|Index Scan using ht_pkey on ht\|Optimizer: .*' | tr '\n' '|')
	errors2=$(q 0 "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'error';")
	[ "$out" = "Seq Scan on ht|Optimizer: GPORCA|Index Scan using ht_pkey on ht|Optimizer: GPORCA|" ] &&
		[ "$errors" = "$errors2" ] \
		&& ok "under ORCA the table's hint is ORCA's -- a scan it would not choose -- and ORCA plans it, nothing raised" \
		|| notok "the hint table under ORCA" "$out / ORCA's errors $errors -> $errors2"

	# and one node, whose own table has its own rows
	for n in 0 1 2; do
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m fast stop >/dev/null 2>&1
	done
	d="$(datadir 0)"
	{
		echo "shared_preload_libraries = '$PRELOAD,gp_orca,gp_ao,pax,pg_hint_plan'"
		echo "unix_socket_directories = '$(sockdir 0)'"
		echo "listen_addresses = ''"
		echo "port = $(port 0)"
	} > "$d/postgresql.auto.conf"
	if "$BINDIR/pg_ctl" -D "$d" -l "$ROOT/node0.log" -w -t 30 start >/dev/null 2>&1; then
		out=$(printf '%s\n' "SET client_min_messages = warning;" \
			"CREATE TABLE sht (id int PRIMARY KEY, val int);" \
			"INSERT INTO sht SELECT i, i % 100 FROM generate_series(1, 10000) i;" "ANALYZE sht;" \
			"INSERT INTO hint_plan.hints (norm_query_string, application_name, hints) VALUES ('EXPLAIN (COSTS false) SELECT * FROM sht WHERE sht.id = ?;', '', 'SeqScan(sht)');" \
			"SET pg_hint_plan.enable_hint_table = on;" \
			"SET gp.optimizer = off;" "EXPLAIN (COSTS false) SELECT * FROM sht WHERE sht.id = 1;" \
			"SET gp.optimizer = on;" "EXPLAIN (COSTS false) SELECT * FROM sht WHERE sht.id = 1;" \
			"SET pg_hint_plan.enable_hint_table = off;" \
			"EXPLAIN (COSTS false) SELECT * FROM sht WHERE sht.id = 1;" | qf 0 |
			grep -o 'Seq Scan on sht\|Index Scan using sht_pkey on sht\|Optimizer: .*' | tr '\n' '|')
		[ "$out" = "Seq Scan on sht|Optimizer: Postgres query optimizer|Seq Scan on sht|Optimizer: GPORCA|Index Scan using sht_pkey on sht|Optimizer: GPORCA|" ] \
			&& ok "on one node the table's hint is the planner's and ORCA's alike, and neither's with the setting off" \
			|| notok "the hint table on one node" "$out"
	else
		notok "one node with pg_hint_plan starts" "$(tail -5 "$ROOT/node0.log")"
	fi
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
