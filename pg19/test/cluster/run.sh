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
	[ "$out" = "0" ] && ok "while the coordinator's own copy is empty" \
		|| notok "the coordinator's copy" "$out"

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
	out=$(printf '%s\n' "CREATE TEMP TABLE tmp1 (a int);" "CREATE TEMP TABLE tmp2 (a int);" \
		"SELECT ('tmp1'::regclass::oid = min(result::oid)) AND ('tmp1'::regclass::oid = max(result::oid)) AND count(*) = 2 FROM gp.exec_on_segments('SELECT ''tmp1''::regclass::oid');" \
		"SELECT ('tmp2'::regclass::oid = min(result::oid)) AND count(*) = 2 FROM gp.exec_on_segments('SELECT ''tmp2''::regclass::oid');" | qf 0)
	[ "$out" = "t
t" ] && ok "temporary tables, two of them, have the coordinator's OIDs" \
		|| notok "temporary tables" "$out"

	out=$(q 0 "VACUUM kept;")
	[ -z "$out" ] && ok "VACUUM, which runs outside a transaction block, reaches the segments too" \
		|| notok "VACUUM" "$out"

	###########################################################################
	echo "8. the segments authenticate the coordinator, with SCRAM"
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
echo "9. a cluster described wrongly is a server that does not start"
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
echo "10. with no cluster configured, this is a single node"
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
else
	notok "a server with no cluster starts" "$(tail -5 "$ROOT/node0.log")"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
