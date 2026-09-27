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
# M7: dump fidelity.  What PostgreSQL 19's own pg_dumpall writes of a
# cluster of the port's -- a coordinator and three segments, every module
# preloaded -- read back by psql into another cluster made the same way; and
# the same of one node, for what only one node has: an incremental view, and
# a directory table's files.  Cloudberry's own pg_dump and pg_dumpall wrote
# its syntax, and never dumped a tag's assignment, a task, a directory table
# or a storage server; stock PostgreSQL tools write labels, extension
# tables and foreign-data objects, which is what the port keeps them as
# ("Proposal: dump fidelity" in cloudberry.md).  What is checked:
#
#   1. the source cluster, with an object of every kind the port has;
#   2. pg_dumpall writes it, saying nothing but the dependency loop a bitmap
#      index's list of values makes (its heap depends on the index, as
#      Cloudberry's does, and pg_dump reads every relation's dependencies,
#      those it does not dump too);
#   3. psql reads it back into a new cluster, saying nothing but that the
#      bootstrap superuser is there already;
#   4. pg_dumpall of the new cluster writes what it wrote of the first, but
#      for what names an OID of the new one -- a directory table's
#      directory, a dynamic table's job;
#   5. and what the objects do there is what they did: where each row is,
#      tags, an index's too, a protocol of the user's, queues, groups and
#      profiles, materialized
#      views populated or not, AO options and encodings, PAX, a bitmap
#      index; and a directory table's rows, each on the segment its path
#      hashes to, and its files, which no dump carries, once each segment's
#      directory is copied to the new cluster's segment of the same content;
#   6. one node: an incremental view's triggers, which the dump does not
#      carry and its label makes again, a dynamic table's job, and a
#      directory table's directory, whose files the dump does not carry;
#   7. and, the ten modules being there, each of their C functions not
#      declared STRICT called with NULLs, which none may crash on.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/dump/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-dump-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbd-XXXXXX)"
BASEPORT="${PGPORT:-$((6500 + RANDOM % 200))}"
PRELOAD='gp_core,gp_orca,gp_sql,gp_matview,gp_task,gp_security,gp_ao,gp_exttable,gp_resource,pax'
MODULES="gp_core gp_sql gp_orca gp_matview gp_task gp_security gp_ao gp_exttable gp_resource pax"
SECRET="dump-fidelity-$RANDOM$RANDOM$RANDOM"
# the bootstrap superuser, whose CREATE ROLE a restore finds done
SUPERUSER="$(id -un)"
# ORCA plans where it can, and its note of a column with no statistics is
# not what is compared
export PGOPTIONS="-c gp.optimizer_print_missing_stats=off"

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | head -12 | sed 's/^/         /'
         fail=$((fail + 1)); }

# A cluster (a coordinator and three segments) or one node, by name.
#   a, b      the clusters, from BASEPORT and BASEPORT+10
#   one, two  single nodes, BASEPORT+20 and BASEPORT+21
nodes_of() { case "$1" in a|b) echo "0 1 2 3" ;; *) echo 0 ;; esac; }
base_of()  { case "$1" in a) echo "$BASEPORT" ;; b) echo $((BASEPORT + 10)) ;;
                          one) echo $((BASEPORT + 20)) ;; two) echo $((BASEPORT + 21)) ;; esac; }
datadir()  { echo "$ROOT/$1/node$2"; }
sockdir()  { echo "$SOCK/$1$2"; }

cleanup() {
	for c in a b one two; do
		for n in $(nodes_of "$c"); do
			[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/$c/node$n.log" "$RESULTS_DIR/dump-$c-node$n.log" 2> /dev/null
			"$BINDIR/pg_ctl" -D "$(datadir "$c" "$n")" -m immediate stop > /dev/null 2>&1
		done
	done
	[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT"/*.sql "$ROOT"/*.out "$RESULTS_DIR/" 2> /dev/null
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
	rm -rf "$SOCK"
}
trap cleanup EXIT

make_server() {				# make_server <name>: initdb, configure and start it
	local c="$1" base; base=$(base_of "$c")
	local conf="$ROOT/$c/gp_cluster.conf"

	mkdir -p "$ROOT/$c"
	if [ "$(nodes_of "$c")" != 0 ]; then
		for n in $(nodes_of "$c"); do
			echo "$((n + 1)) $((n - 1)) p $(sockdir "$c" "$n") $((base + n)) $(datadir "$c" "$n")"
		done > "$conf"
	fi
	for n in $(nodes_of "$c"); do
		mkdir -p "$(sockdir "$c" "$n")"
		"$BINDIR/initdb" -D "$(datadir "$c" "$n")" -N --locale=C --encoding=UTF8 \
			> "$ROOT/$c/initdb$n.log" 2>&1 \
			|| { echo "initdb failed for $c node $n"; tail -20 "$ROOT/$c/initdb$n.log"; exit 1; }
		{
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "unix_socket_directories = '$(sockdir "$c" "$n")'"
			echo "listen_addresses = ''"
			echo "port = $((base + n))"
			echo "fsync = off"
			echo "max_prepared_transactions = 32"
			if [ -f "$conf" ]; then
				echo "gp.cluster_config = '$conf'"
				echo "gp.dbid = $((n + 1))"
				echo "gp.cluster_secret = '$SECRET'"
				[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
			fi
		} >> "$(datadir "$c" "$n")/postgresql.auto.conf"
	done
	for n in $(nodes_of "$c" | tr ' ' '\n' | sort -r); do
		"$BINDIR/pg_ctl" -D "$(datadir "$c" "$n")" -l "$ROOT/$c/node$n.log" -w -t 60 start > /dev/null 2>&1 \
			|| { echo "$c node $n did not start"; tail -20 "$ROOT/$c/node$n.log"; exit 1; }
	done
}

q() {						# q <server> <db> <sql>: on its coordinator
	"$PSQL" -X -q -t -A -h "$(sockdir "$1" 0)" -p "$(base_of "$1")" -d "$2" -c "$3" 2>&1
}
qf() {						# qf <server> <db> <file>, as psql reads a script
	"$PSQL" -X -h "$(sockdir "$1" 0)" -p "$(base_of "$1")" -d "$2" -f "$3" 2>&1
}
dumpall() {					# dumpall <server> <file>: stderr beside it
	"$BINDIR/pg_dumpall" -h "$(sockdir "$1" 0)" -p "$(base_of "$1")" > "$2" 2> "$2.err"
}
is() {						# is <name> <server> <db> <sql> <want>
	local got; got=$(q "$2" "$3" "$4")
	[ "$got" = "$5" ] && ok "$1" || notok "$1" "want [$5], got [$got]"
}
same() {					# same <name> <db> <sql>: the same on a and b
	local a b; a=$(q a "$2" "$3"); b=$(q b "$2" "$3")
	[ "$a" = "$b" ] && ok "$1" || notok "$1" "$(diff <(printf '%s\n' "$a") <(printf '%s\n' "$b"))"
}

# A dump with every COPY's rows in order, which a gather from the segments
# does not keep, without the psql \restrict keys, and with the names that
# carry an OID of the database the dump was made of written as X: a
# directory table's directory and a dynamic table's job, which the restore
# made again under the new ones, and the sequence of job IDs the job took.
normalize() {
	python3 - "$1" << 'EOF'
import re, sys
out, block = [], None
for line in open(sys.argv[1], encoding='utf-8'):
    if line.startswith(('\\restrict', '\\unrestrict')):
        continue
    line = re.sub(r'directory_location=base/[0-9]+/[0-9]+_dirtable',
                  'directory_location=base/X/X_dirtable', line)
    line = re.sub(r'gp_dynamic_table_refresh_[0-9]+', 'gp_dynamic_table_refresh_X', line)
    line = re.sub(r"setval\('gp_task\.job_jobid_seq', [0-9]+", "setval('gp_task.job_jobid_seq', X", line)
    if block is not None:
        if line == '\\.\n':
            out.extend(sorted(block)); out.append(line); block = None
        else:
            block.append(line)
        continue
    # an object's labels, one line each, come in the order pg_seclabel's
    # rows are read, which a restore need not keep
    if line.startswith('SECURITY LABEL FOR ') and out and out[-1].startswith('SECURITY LABEL FOR '):
        run = len(out)
        while run > 0 and out[run - 1].startswith('SECURITY LABEL FOR '):
            run -= 1
        out[run:] = sorted(out[run:] + [line])
        continue
    out.append(line)
    if line.startswith('COPY ') and line.rstrip().endswith('FROM stdin;'):
        block = []
sys.stdout.write(''.join(out))
EOF
}

# What pg_dump says of a bitmap index, and only that: see the head comment.
only_bitmap_loop() {
	grep -v -e '^pg_dump: warning: could not resolve dependency loop among these items:$' \
	        -e '^pg_dump: detail: ' "$1"
	grep '^pg_dump: detail: ' "$1" | grep -v -e 'TABLE pg_bm_[0-9]* ' -e 'INDEX ao1_b ' \
		-e 'POST-DATA BOUNDARY' -e 'PRE-DATA BOUNDARY' -e 'TABLE DATA '
}

echo "M7 dump fidelity"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

###############################################################################
echo "1. a cluster with an object of every kind the port has"
###############################################################################
make_server a
for db in postgres src; do
	[ "$db" = src ] && q a postgres "CREATE DATABASE src" > /dev/null
	for m in $MODULES; do
		# a database the coordinator makes has gp_core already (gp_ddl.c)
		[ "$db" = src ] && [ "$m" = gp_core ] && continue
		out=$(q a "$db" "CREATE EXTENSION $m")
		[ -n "$out" ] && { echo "CREATE EXTENSION $m in $db: $out"; exit 1; }
	done
done

cat > "$ROOT/source.sql" << 'EOF'
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
-- distributed tables, every policy, a legacy hash class and a key of two
CREATE TABLE t_hash (a int, b text) DISTRIBUTED BY (a);
CREATE TABLE t_rand (a int, b text) DISTRIBUTED RANDOMLY;
CREATE TABLE t_repl (a int, b text) DISTRIBUTED REPLICATED;
CREATE TABLE t_legacy (a int, b text) DISTRIBUTED BY (a cdbhash_int4_ops);
CREATE TABLE t_pk (id int PRIMARY KEY, v text);
CREATE TABLE t_two (a int, b int, c text) DISTRIBUTED BY (b, a);
INSERT INTO t_hash SELECT g, 'h' || g FROM generate_series(1, 100) g;
INSERT INTO t_rand SELECT g, 'r' || g FROM generate_series(1, 100) g;
INSERT INTO t_repl SELECT g, 'p' || g FROM generate_series(1, 10) g;
INSERT INTO t_legacy SELECT g, 'l' || g FROM generate_series(1, 100) g;
INSERT INTO t_pk SELECT g, 'k' || g FROM generate_series(1, 100) g;
INSERT INTO t_two SELECT g, g % 7, 'x' FROM generate_series(1, 100) g;
-- Cloudberry's classic partitions, with a subpartition template
CREATE TABLE sales (id int, d date, amt numeric) DISTRIBUTED BY (id)
  PARTITION BY RANGE (d) SUBPARTITION BY LIST (id)
  SUBPARTITION TEMPLATE (SUBPARTITION low VALUES (1, 2, 3), DEFAULT SUBPARTITION hi)
  (START (date '2024-01-01') INCLUSIVE END (date '2024-04-01') EXCLUSIVE
   EVERY (INTERVAL '1 month'), DEFAULT PARTITION other);
INSERT INTO sales SELECT g, date '2024-01-01' + g, g FROM generate_series(1, 200) g;
-- append-optimized, by row and by column, a column added, a bitmap index
CREATE TABLE ao1 (a int, b text)
  WITH (appendonly=true, compresstype=zlib, compresslevel=5) DISTRIBUTED BY (a);
CREATE TABLE aoco1 (a int ENCODING (compresstype=zstd), b text)
  WITH (appendonly=true, orientation=column) DISTRIBUTED BY (a);
INSERT INTO ao1 SELECT g, 'a' || g FROM generate_series(1, 1000) g;
INSERT INTO aoco1 SELECT g, 'c' || g FROM generate_series(1, 1000) g;
ALTER TABLE aoco1 ADD COLUMN c int DEFAULT 5 ENCODING (compresstype=rle_type);
CREATE INDEX ao1_b ON ao1 USING bitmap (b);
DELETE FROM ao1 WHERE a % 10 = 0;
-- PAX
CREATE TABLE px (a int, b text) USING pax WITH (minmax_columns='a') DISTRIBUTED BY (a);
INSERT INTO px SELECT g, 'x' || g FROM generate_series(1, 1000) g;
-- external tables
CREATE EXTERNAL WEB TABLE ext_exec (a text) EXECUTE 'echo hello' FORMAT 'text';
CREATE READABLE EXTERNAL TABLE ext_file (a int, b text)
  LOCATION ('file://localhost/nonexistent.txt') FORMAT 'csv'
  LOG ERRORS SEGMENT REJECT LIMIT 10 ROWS;
CREATE WRITABLE EXTERNAL WEB TABLE ext_w (a int) EXECUTE 'cat > /dev/null'
  FORMAT 'text' DISTRIBUTED BY (a);
-- where a function runs, and what it does with SQL
CREATE FUNCTION f_segs() RETURNS SETOF int AS $$ SELECT 1 $$ LANGUAGE sql EXECUTE ON ALL SEGMENTS;
CREATE FUNCTION f_coord() RETURNS SETOF int AS $$ SELECT 1 $$ LANGUAGE sql EXECUTE ON COORDINATOR;
CREATE FUNCTION f_reads() RETURNS int AS $$ SELECT 1 $$ LANGUAGE sql READS SQL DATA;
-- materialized views: hashed, replicated, not populated, and a dynamic table
CREATE MATERIALIZED VIEW mv AS SELECT a, b FROM t_hash DISTRIBUTED BY (a);
CREATE UNIQUE INDEX mv_a ON mv (a);
CREATE MATERIALIZED VIEW mv_repl AS SELECT a FROM t_repl DISTRIBUTED REPLICATED;
CREATE MATERIALIZED VIEW mv_empty AS SELECT a FROM t_hash WITH NO DATA;
CREATE DYNAMIC TABLE dt SCHEDULE '0 3 * * *' AS SELECT count(*) AS n FROM t_hash;
-- a task, in the task database
CREATE TASK nightly SCHEDULE '0 2 * * *' AS 'VACUUM';
-- tags, one of them an ordinary role's, on tables and an index
CREATE TAG env ALLOWED_VALUES 'prod', 'dev';
CREATE TAG owner_team;
ALTER TABLE t_hash TAG (env = 'prod', owner_team = 'geo');
CREATE TABLE t_tagged (a int) TAG (env = 'dev');
CREATE INDEX t_hash_b ON t_hash (b);
ALTER INDEX t_hash_b TAG (owner_team = 'index');
-- a storage server, a mapping, and a directory table with files, each on
-- the segment its path hashes to
CREATE STORAGE SERVER s3 OPTIONS (endpoint 's3.example.com');
CREATE STORAGE USER MAPPING FOR CURRENT_USER STORAGE SERVER s3 OPTIONS (accesskey 'k', secretkey 's');
CREATE DIRECTORY TABLE docs;
SELECT gp_sql.directory_table_put('docs'::regclass, 'f' || g || '.txt', convert_to('file ' || g, 'UTF8'),
                                  CASE WHEN g % 2 = 0 THEN 'even' END)
  FROM generate_series(1, 9) g;
-- roles: a profile, a queue, a group, DENY windows, tags roles own
CREATE PROFILE strict LIMIT FAILED_LOGIN_ATTEMPTS 3 PASSWORD_LOCK_TIME 1;
CREATE RESOURCE QUEUE rq1 WITH (active_statements=3);
CREATE RESOURCE GROUP rg1 WITH (concurrency=5, cpu_max_percent=20);
CREATE ROLE u1 LOGIN RESOURCE QUEUE rq1 RESOURCE GROUP rg1 DENY DAY 'Tuesday';
ALTER ROLE u1 PROFILE strict;
CREATE ROLE aaa LOGIN RESOURCE QUEUE rq1;
CREATE TAG aaa_tag;
ALTER TAG aaa_tag OWNER TO aaa;
CREATE ROLE zzz LOGIN;
CREATE TAG zzz_tag ALLOWED_VALUES 'a';
ALTER TAG zzz_tag OWNER TO zzz;
-- a protocol of the user's, given to a role, a privilege on it granted, and
-- an external table of it
CREATE FUNCTION write_to_file() RETURNS integer AS '$libdir/gpextprotocol.so', 'demoprot_export' LANGUAGE C STABLE NO SQL;
CREATE FUNCTION read_from_file() RETURNS integer AS '$libdir/gpextprotocol.so', 'demoprot_import' LANGUAGE C STABLE NO SQL;
CREATE TRUSTED PROTOCOL demoprot (readfunc = 'read_from_file', writefunc = 'write_to_file');
ALTER PROTOCOL demoprot OWNER TO zzz;
GRANT SELECT ON PROTOCOL demoprot TO aaa;
CREATE EXTERNAL TABLE ext_demo (a int) LOCATION ('demoprot://demo.txt') FORMAT 'text';
EOF
out=$(qf a src "$ROOT/source.sql")
case "$out" in
	*ERROR*) notok "every object is made" "$out" ;;
	*) ok "every object is made: tables of each policy, partitions, AO, PAX, external tables, materialized views, a task, tags, a directory table, profiles, queues and groups" ;;
esac
# the role that holds a profile is made quietly: Cloudberry's CREATE PROFILE
# makes none, and says nothing of a queue or a group
out=$(q a src "CREATE PROFILE quiet LIMIT FAILED_LOGIN_ATTEMPTS 2; SELECT 'made'")
[ "$out" = made ] && ok "CREATE PROFILE says nothing of the queue its role would get" \
	|| notok "CREATE PROFILE's messages" "$out"

###############################################################################
echo "2. pg_dumpall writes it"
###############################################################################
dumpall a "$ROOT/a.sql"; rc=$?
[ "$rc" -eq 0 ] && ok "pg_dumpall of the cluster succeeds" || notok "pg_dumpall" "$(cat "$ROOT/a.sql.err")"
extra=$(only_bitmap_loop "$ROOT/a.sql.err")
[ -z "$extra" ] && ok "it says nothing but the bitmap index's dependency loop" \
	|| notok "pg_dumpall's messages" "$extra"
out=$(grep -c -e 'CREATE SCHEMA gp_ao;' -e 'CREATE SCHEMA gp_sql;' "$ROOT/a.sql")
[ "$out" -ge 4 ] && ok "the extensions' schemas are written before the extensions, as for any extension" \
	|| notok "the extensions' schemas" "$out"
out=$(grep -c 'pg_pax_blocks' "$ROOT/a.sql")
[ "$out" = 0 ] && ok "no PAX table's aux table is written: they are in pg_ext_aux" \
	|| notok "PAX's aux tables" "$(grep 'pg_pax_blocks' "$ROOT/a.sql" | head -3)"
out=$(grep -c -e "^SECURITY LABEL FOR gp_index_tag ON TABLE public.t_hash IS '{\"t_hash_b\": {\"owner_team\": \"index\"}}';" "$ROOT/a.sql")
[ "$out" = 1 ] && ok "an index's tags are its table's label, by the index's name, written with the table" \
	|| notok "index tags" "$(grep 'gp_index_tag' "$ROOT/a.sql")"
out=$(grep -c "^SECURITY LABEL FOR gp_protocol ON FUNCTION public.read_from_file() IS '{\"demoprot\": {" "$ROOT/a.sql")
[ "$out" = 1 ] && ok "a protocol is a label on each of its functions, written with the function" \
	|| notok "a protocol's label" "$(grep 'gp_protocol' "$ROOT/a.sql")"
out=$(grep -c 'gp_dynamic_table_refresh_' "$ROOT/a.sql")
[ "$out" = 0 ] && ok "nor a dynamic table's job, which its label makes again" \
	|| notok "a dynamic table's job" "$(grep 'gp_dynamic_table_refresh_' "$ROOT/a.sql")"
out=$(grep -e "SECURITY LABEL FOR gp_tag_definitions" "$ROOT/a.sql")
case "$out" in
	*'"owner": "aaa"'*'"owner": "zzz"'*) ok "a tag's owner is written by name" ;;
	*) notok "the tag definitions' owners" "$out" ;;
esac
out=$(grep -e "SECURITY LABEL FOR gp ON MATERIALIZED VIEW public.mv IS" -e "REFRESH MATERIALIZED VIEW public.mv;" "$ROOT/a.sql")
case "$out" in
	*"distributed_by=(a)"*"REFRESH MATERIALIZED VIEW public.mv;"*)
		ok "a materialized view with its distribution, filled at the end by REFRESH" ;;
	*) notok "a materialized view" "$out" ;;
esac

###############################################################################
echo "3. psql reads it back into a new cluster"
###############################################################################
make_server b
"$PSQL" -X -h "$(sockdir b 0)" -p "$(base_of b)" -d postgres -f "$ROOT/a.sql" > "$ROOT/restore.out" 2>&1
out=$(grep -E 'ERROR|FATAL|WARNING' "$ROOT/restore.out" | grep -v "role \"$SUPERUSER\" already exists")
[ -z "$out" ] && ok "the restore says nothing but that the bootstrap superuser is there" \
	|| notok "the restore" "$(grep -E -A3 'ERROR|FATAL|WARNING' "$ROOT/restore.out" | grep -v "role \"$SUPERUSER\" already exists" | head -12)"

###############################################################################
echo "4. pg_dumpall of the new cluster writes what it wrote of the first"
###############################################################################
dumpall b "$ROOT/b.sql"
extra=$(only_bitmap_loop "$ROOT/b.sql.err")
[ -z "$extra" ] && ok "pg_dumpall of the new cluster says as little" || notok "the second dump's messages" "$extra"
normalize "$ROOT/a.sql" > "$ROOT/a.norm.sql"
normalize "$ROOT/b.sql" > "$ROOT/b.norm.sql"
out=$(diff "$ROOT/a.norm.sql" "$ROOT/b.norm.sql")
[ -z "$out" ] && ok "the two dumps are the same, rows and all, but for the new cluster's OIDs" \
	|| notok "the two dumps" "$out"

###############################################################################
echo "5. and the objects do what they did"
###############################################################################
same "every table's distribution, and the partitions'" src \
     "SELECT localoid::regclass::text, policytype, numsegments, distkey::text FROM gp_distribution_policy ORDER BY 1"
same "and each hashed table's rows on the segments they were on" src \
     "SELECT 't_hash', gp_segment_id, count(*) FROM t_hash GROUP BY 2
      UNION ALL SELECT 't_legacy', gp_segment_id, count(*) FROM t_legacy GROUP BY 2
      UNION ALL SELECT 't_two', gp_segment_id, count(*) FROM t_two GROUP BY 2
      UNION ALL SELECT 'sales', gp_segment_id, count(*) FROM sales GROUP BY 2
      UNION ALL SELECT 'ao1', gp_segment_id, count(*) FROM ao1 GROUP BY 2
      UNION ALL SELECT 'aoco1', gp_segment_id, count(*) FROM aoco1 GROUP BY 2
      UNION ALL SELECT 'px', gp_segment_id, count(*) FROM px GROUP BY 2
      UNION ALL SELECT 'mv', gp_segment_id, count(*) FROM mv GROUP BY 2 ORDER BY 1, 2"
same "a replicated table's on every segment" src \
     "SELECT count(*) FROM gp_dist_random('t_repl')"
same "access methods and their options" src \
     "SELECT c.relname, a.amname, c.reloptions FROM pg_class c JOIN pg_am a ON a.oid = c.relam
       WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r', 'm') ORDER BY 1"
same "a column's encoding" src \
     "SELECT attnum, filenum, attoptions::text FROM pg_attribute_encoding WHERE attrelid = 'aoco1'::regclass ORDER BY 1"
same "an AO table's rows, read through its bitmap index" src \
     "SET enable_seqscan = off; SELECT count(*), sum(a) FROM ao1 WHERE b IN ('a55', 'a56', 'a60')"
same "a column added with a default" src "SELECT count(*), sum(c) FROM aoco1"
same "where each function runs, and what it does with SQL" src \
     "SELECT objname, label FROM pg_seclabels WHERE provider = 'gp' AND objtype = 'function' ORDER BY 1"
# pg_dump writes a foreign table's options in the order of their names
same "an external table's options" src \
     "SELECT c.relname, array_to_string(ARRAY(SELECT o FROM unnest(t.ftoptions) o ORDER BY o), ',')
        FROM pg_foreign_table t JOIN pg_class c ON c.oid = t.ftrelid ORDER BY 1"
same "the tags on tables" src \
     "SELECT objname, label FROM pg_seclabels WHERE provider = 'gp_tag' ORDER BY 1"
same "and an index's, on the index made again under another OID" src \
     "SELECT indexrelid::regclass::text, tagname, tagvalue FROM gp_sql.index_tag ORDER BY 1, 2"
same "a protocol of the user's: its functions, trust, owner and privileges" src \
     "SELECT ptcname, ptcreadfn, ptcwritefn, ptcvalidatorfn, pg_get_userbyid(ptcowner), ptctrusted, ptcacl
        FROM pg_extprotocol ORDER BY 1"
is "on every segment too" b src \
   "SELECT string_agg(result, ',' ORDER BY content) FROM gp.exec_on_segments(
      'SELECT ptcname || '' '' || pg_get_userbyid(ptcowner) || '' '' || ptcacl::text FROM pg_extprotocol')" \
   "demoprot zzz {zzz=ar/zzz,aaa=r/zzz},demoprot zzz {zzz=ar/zzz,aaa=r/zzz},demoprot zzz {zzz=ar/zzz,aaa=r/zzz}"
same "the tags' definitions, each owned by the role it was" postgres \
     "SELECT tagname, pg_get_userbyid(tagowner), allowed_values FROM pg_tag ORDER BY 1"
same "each role's queue, group, profile and DENY windows" postgres \
     "SELECT r.rolname, q.rsqname, g.rsgname, s.label
        FROM pg_roles r LEFT JOIN pg_resqueue q ON q.oid = r.rolresqueue
        LEFT JOIN pg_resgroup g ON g.oid = r.rolresgroup
        LEFT JOIN pg_shseclabel s ON s.objoid = r.oid AND s.provider = 'gp'
       WHERE r.rolname IN ('u1', 'aaa', 'zzz') ORDER BY 1"
same "the queues and groups themselves" postgres \
     "SELECT rsqname, rsqcountlimit FROM pg_resqueue UNION ALL
      SELECT rsgname, NULL FROM pg_resgroup ORDER BY 1"
same "materialized views, populated or not, and their rows" src \
     "SELECT relname, relispopulated FROM pg_class WHERE relkind = 'm' ORDER BY 1;
      SELECT count(*), sum(a) FROM mv; SELECT count(*) FROM gp_dist_random('mv_repl'); SELECT n FROM dt"
same "a storage server and the mapping of its user" src \
     "SELECT srvname, array_to_string(srvoptions, ',') FROM pg_foreign_server WHERE srvname = 's3';
      SELECT usename, array_to_string(umoptions, ',') FROM pg_user_mappings WHERE srvname = 's3'"
same "the tasks, a dynamic table's among them" postgres \
     "SELECT regexp_replace(jobname, '[0-9]+\$', 'X'), schedule, command, username FROM pg_task ORDER BY 1"
# the view is in src, its job in the task database, postgres
out=$(q b src "SELECT 'gp_dynamic_table_refresh_' || 'dt'::regclass::oid")
out2=$(q b postgres "SELECT jobname FROM pg_task WHERE jobname LIKE 'gp_dynamic%'")
[ "$out" = "$out2" ] && ok "the dynamic table's job is named by its new OID" \
	|| notok "the dynamic table's job" "$out / $out2"
out=$(q b src "SELECT gp_sql.directory_table_location('docs'::regclass) = 'base/' ||
                (SELECT oid FROM pg_database WHERE datname = 'src') || '/' || 'docs'::regclass::oid || '_dirtable'")
[ "$out" = t ] && ok "a directory table's directory is its new database's and OID's" \
	|| notok "a directory table's directory" "$out"
same "a directory table's rows, each on the segment its path hashes to" src \
     "SELECT gp_segment_id, relative_path, size, md5, tag FROM docs ORDER BY 2"
is "and not its files, which no dump carries" b src \
   "SELECT count(*) FROM directory_table('docs') WHERE content IS NULL" "9"
# Each segment's directory of the table copied to the new cluster's segment
# of the same content, where its rows are.
old=$(q a src "SELECT gp_sql.directory_table_location('docs'::regclass)")
new=$(q b src "SELECT gp_sql.directory_table_location('docs'::regclass)")
for n in 1 2 3; do
	if [ -d "$(datadir a "$n")/$old" ]; then
		mkdir -p "$(datadir b "$n")/$new"
		cp -r "$(datadir a "$n")/$old/." "$(datadir b "$n")/$new/"
	fi
done
same "until each segment's directory is copied to the segment of the same content" src \
     "SELECT relative_path, convert_from(content, 'UTF8'), md5(content) = md5 FROM directory_table('docs') ORDER BY 1"
out=$(q b src "REFRESH MATERIALIZED VIEW CONCURRENTLY mv; SELECT count(*) FROM mv;
               REFRESH MATERIALIZED VIEW mv_empty; SELECT count(*) FROM mv_empty")
[ "$out" = "100
100" ] && ok "a restored view refreshes, concurrently too" || notok "refreshing a restored view" "$out"
# CONCURRENTLY writes only the rows that changed, as PostgreSQL's does: each
# segment's rows the new data has too stay where they are
where="SELECT string_agg(gp_segment_id || ':' || ctid::text || ':' || a, ',' ORDER BY a) FROM mv WHERE a BETWEEN 3 AND 100"
kept=$(q b src "$where")
q b src "UPDATE t_hash SET b = 'changed' WHERE a = 1; DELETE FROM t_hash WHERE a = 2;
         INSERT INTO t_hash VALUES (1001, 'new')" > /dev/null
out=$(q b src "REFRESH MATERIALIZED VIEW CONCURRENTLY mv;
               SELECT count(*) FROM ((SELECT a, b FROM t_hash EXCEPT SELECT a, b FROM mv)
                                     UNION ALL (SELECT a, b FROM mv EXCEPT SELECT a, b FROM t_hash)) d;
               SELECT b FROM mv WHERE a = 1")
[ "$out" = "0
changed" ] && [ "$(q b src "$where")" = "$kept" ] \
	&& ok "and a concurrent refresh writes only the rows that changed, the others where they were" \
	|| notok "a concurrent refresh's rows" "$out"
out=$(q b src "INSERT INTO t_hash VALUES (1001, 'new'); REFRESH MATERIALIZED VIEW CONCURRENTLY mv" 2>&1)
case "$out" in
	*"new data for materialized view \"mv\" contains duplicate rows without any null columns"*)
		ok "new data with a row twice is refused in PostgreSQL's words" ;;
	*) notok "a concurrent refresh of duplicate rows" "$out" ;;
esac

###############################################################################
echo "6. one node: an incremental view, a dynamic table and a directory table's files"
###############################################################################
make_server one
q one postgres "CREATE DATABASE src" > /dev/null
for db in postgres src; do
	for m in gp_core gp_sql gp_matview gp_task; do q one "$db" "CREATE EXTENSION $m" > /dev/null; done
done
cat > "$ROOT/one.sql" << 'EOF'
\set ON_ERROR_STOP 1
CREATE TABLE base (a int, b int);
INSERT INTO base SELECT g, g % 5 FROM generate_series(1, 100) g;
CREATE INCREMENTAL MATERIALIZED VIEW imv AS SELECT b, count(*) AS n, sum(a) AS s FROM base GROUP BY b;
CREATE DYNAMIC TABLE dt SCHEDULE '0 3 * * *' AS SELECT count(*) AS n FROM base;
CREATE DIRECTORY TABLE docs;
SELECT gp_sql.directory_table_put('docs'::regclass, 'a/b.txt', 'hello'::bytea, 'greeting');
EOF
out=$(qf one src "$ROOT/one.sql")
case "$out" in *ERROR*) notok "the node's objects are made" "$out" ;; *) ok "an incremental view, a dynamic table and a directory table with a file are made" ;; esac
is "the view's triggers are internal, as Cloudberry's are" one src \
   "SELECT count(*) FILTER (WHERE tgisinternal), count(*) FROM pg_trigger WHERE tgrelid = 'base'::regclass" "8|8"
dumpall one "$ROOT/one.sql.dump"
out=$(grep -c -e 'CREATE TRIGGER' -e 'gp_dynamic_table_refresh' "$ROOT/one.sql.dump")
[ "$out" = 0 ] && ok "pg_dumpall writes neither the triggers nor the dynamic table's job" \
	|| notok "triggers and jobs in the dump" "$(grep -e 'CREATE TRIGGER' -e 'gp_dynamic_table_refresh' "$ROOT/one.sql.dump")"
make_server two
"$PSQL" -X -h "$(sockdir two 0)" -p "$(base_of two)" -d postgres -f "$ROOT/one.sql.dump" > "$ROOT/two.out" 2>&1
out=$(grep -E 'ERROR|FATAL|WARNING' "$ROOT/two.out" | grep -v "role \"$SUPERUSER\" already exists")
[ -z "$out" ] && ok "psql reads it back" || notok "the node's restore" "$out"
is "the restored label makes the view's triggers again, under its new OID" two src \
   "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'base'::regclass AND tgisinternal
      AND encode(tgargs, 'escape') = 'imv'::regclass::oid::text || '\000'" "8"
is "and the view keeps up with its base table" two src \
   "INSERT INTO base VALUES (1000, 1); SELECT n, s FROM imv WHERE b = 1" "21|1970"
out=$(q two src "SELECT 'gp_dynamic_table_refresh_' || 'dt'::regclass::oid")
out2=$(q two postgres "SELECT jobname || ' ' || command FROM gp_task.job WHERE jobname LIKE 'gp_dynamic%'")
[ "$out2" = "$out REFRESH MATERIALIZED VIEW public.dt" ] && ok "the dynamic table's job is made again, under its new OID" \
	|| notok "the dynamic table's job" "$out / $out2"
old=$(q one src "SELECT gp_sql.directory_table_location('docs'::regclass)")
new=$(q two src "SELECT gp_sql.directory_table_location('docs'::regclass)")
is "a directory table's row comes back, and its file does not" two src \
   "SELECT relative_path, size, tag, gp_sql.directory_table_get('docs'::regclass, relative_path) IS NULL FROM docs" "a/b.txt|5|greeting|t"
cp -r "$(datadir one 0)/$old/." "$(datadir two 0)/$new/"
is "until the old directory's files are copied into its new one" two src \
   "SELECT convert_from(gp_sql.directory_table_get('docs'::regclass, 'a/b.txt'), 'UTF8')" "hello"

###############################################################################
echo "7. no C function of the modules crashes the server on a NULL argument"
###############################################################################
# Each C function and procedure of the ten modules that is not STRICT and
# takes an argument, called with every argument NULL -- by a role of no
# privilege, and by a superuser in a transaction rolled back -- errs or
# answers, and never loses its connection: seventeen of them read a NULL as a
# pointer, and the postmaster restarted every session of the node.  A
# polymorphic argument is given an int's NULL, an array one an int[]'s; one
# of type internal, which SQL cannot pass, is not called.
q a src "CREATE ROLE sweeper LOGIN" > /dev/null
q a src "
SELECT CASE p.prokind WHEN 'p' THEN 'CALL ' ELSE 'SELECT ' END
       || quote_ident(n.nspname) || '.' || quote_ident(p.proname) || '('
       || (SELECT string_agg(CASE WHEN t.typname IN ('anyelement', 'anynonarray', 'anycompatible', 'any')
                                  THEN 'NULL::int4'
                                  WHEN t.typname IN ('anyarray', 'anycompatiblearray') THEN 'NULL::int4[]'
                                  ELSE 'NULL::' || format_type(a.t, NULL) END, ', ' ORDER BY a.i)
             FROM unnest(p.proargtypes::oid[]) WITH ORDINALITY a(t, i) JOIN pg_type t ON t.oid = a.t)
       || ')'
  FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE p.prolang = (SELECT oid FROM pg_language WHERE lanname = 'c')
   AND NOT p.proisstrict AND p.pronargs > 0
   AND NOT 'internal'::regtype = ANY (p.proargtypes::oid[])
   AND EXISTS (SELECT FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid
                WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype = 'e'
                  AND e.extname = ANY (string_to_array('$MODULES', ' ')))
 ORDER BY 1" > "$ROOT/sweep.calls"
sweep() {					# sweep <role> <sql>: on a's coordinator, in src
	PGOPTIONS="$PGOPTIONS -c statement_timeout=60s" \
	"$PSQL" -X -q -t -A -h "$(sockdir a 0)" -p "$(base_of a)" -d src -U "$1" -c "$2" 2>&1
}
lost=""
while read -r call; do
	for as in sweeper "$SUPERUSER"; do
		[ "$as" = sweeper ] && sql="$call" || sql="BEGIN; $call; ROLLBACK;"
		out=$(sweep "$as" "$sql")
		case "$out" in
			*"server closed the connection"*|*"connection to server"*|*"terminating connection"*|*"recovery mode"*)
				lost="$lost$as: $call: $(printf '%s\n' "$out" | head -1)"$'\n' ;;
		esac
	done
done < "$ROOT/sweep.calls"
calls=$(grep -c . "$ROOT/sweep.calls")
grep -q '^SELECT gp_sql\.directory_table_put(' "$ROOT/sweep.calls" && [ -z "$lost" ] \
	&& ok "each of the $calls errs or answers, by either role, with the connection kept" \
	|| notok "$calls functions called with NULLs" "${lost:-directory_table_put() was not among them}"
out=$(grep -H -e 'terminated by signal' -e 'terminated by exception' "$ROOT"/a/node*.log)
[ -z "$out" ] && ok "and no process of the cluster ended on a signal" || notok "a process ended on a signal" "$out"
refused_null() {			# refused_null <name> <sql> <message>: as the superuser
	local got; got=$(q a src "$2")
	case "$got" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$3], got [$got]" ;;
	esac
}
refused_null "a NULL option of a statement's CALL is refused, as PostgreSQL refuses one" \
	"CALL gp_resource.create_resource_queue('q_null', ARRAY['active_statements=i:2', NULL])" \
	"null array element not allowed in this context"
refused_null "ALTER RESOURCE GROUP's CALL of no option is refused, where it read the first of none" \
	"CALL gp_resource.alter_resource_group('admin_group', ARRAY[]::text[])" \
	"ALTER RESOURCE GROUP sets one option, not 0"
is "pg_resgroup_move_query() answers NULL for a NULL, as Cloudberry's, strict, does" a src \
   "SELECT pg_resgroup_move_query(NULL, 'admin_group') IS NULL" "t"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
