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
# Append-optimized tables: what the port's storage does that Cloudberry's
# tests cannot see.
#
# Cloudberry's own tests of append-optimized tables run in the singlenode
# suite, against their expected output.  These ask what the port does
# differently underneath: the rows are in 8K pages of the table's relation,
# logged by gp_ao's resource manager, so a crash, a standby and pg_checksums
# see them as any relation's; the metadata is in gp_ao's three tables, keyed
# by the storage ID the first page holds; row numbers and TIDs are the
# port's; and the core patches gp_ao asks (O13-O18) are used as they were
# meant to be.  And a PAX table's ENCODING clauses, which gp_ao takes for it
# and PAX checks and writes by (section 15).
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/ao/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-ao-XXXXXX")"
SOCK="$(mktemp -d /tmp/cba-XXXXXX)"
PORT="${PGPORT:-$((7300 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK"

pass=0; fail=0
ok()    { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok() { printf '  NOT OK %s\n' "$1"
          [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/         /'
          fail=$((fail + 1)); }

cleanup() {
	exec 7>&- 8>&- 2> /dev/null
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	"$BINDIR/pg_ctl" -D "$WORK/standby" -m immediate stop > /dev/null 2>&1
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

q()  { "$PSQL" -X -q -t -A -d postgres -c "$1" 2>&1; }

is() {
	local got; got=$(q "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

isl() {
	local got; got=$(q "$2" | grep -v '^$' | tail -1)
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

refused() {
	local got; got=$(q "$2")
	case "$got" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$3], got [$got]" ;;
	esac
}

# A second and a third session, kept open, for what one session holding a
# transaction open does to another: each reads its statements from a pipe.
session() {
	local n="$1" fd="$2"
	mkfifo "$WORK/s$n.in"
	PGAPPNAME="s$n" "$PSQL" -X -q -t -A -d postgres < "$WORK/s$n.in" > "$WORK/s$n.out" 2>&1 &
	eval "exec $fd> \"$WORK/s$n.in\""
}
# Until session n is idle, in a transaction or not, having run what it was sent.
settled() {
	for _ in $(seq 100); do
		[ "$(q "SELECT count(*) FROM pg_stat_activity
				 WHERE application_name = '$1' AND state LIKE 'idle%';")" = 1 ] && return 0
		sleep 0.1
	done
	return 1
}
# Until a statement that starts so waits for a lock.
waits() {
	for _ in $(seq 100); do
		[ "$(q "SELECT count(*) FROM pg_stat_activity
				 WHERE wait_event_type = 'Lock' AND query LIKE '$1%';")" = 1 ] && return 0
		sleep 0.1
	done
	return 1
}

echo "append-optimized tables: rows in blocks, in 8K pages of their relation"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N -k --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "shared_preload_libraries = 'gp_core,gp_sql,gp_ao,pax'"
	echo "wal_level = replica"
	echo "max_wal_senders = 4"
} >> "$WORK/data/postgresql.conf"
echo "local replication all trust" >> "$WORK/data/pg_hba.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_ao CASCADE; CREATE EXTENSION gp_sql;")
case "$out" in
	*ERROR*) echo "the extension could not be created:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. two access methods, and Cloudberry's spelling of them"
###############################################################################
is "ao_row and ao_column are table access methods" \
   "SELECT string_agg(amname || ':' || amtype::text, ',' ORDER BY amname) FROM pg_am
     WHERE amname LIKE 'ao\_%';" "ao_column:t,ao_row:t"
q "CREATE TABLE r1 (a int, b text) WITH (appendonly=true);
   CREATE TABLE c1 (a int, b text) WITH (appendoptimized=true, orientation=column);
   CREATE TABLE h1 (a int) WITH (appendonly=false);" > /dev/null
is "appendonly and orientation choose the method, and are not kept" \
   "SELECT string_agg(relname || ':' || amname || ':' || coalesce(array_to_string(reloptions, ' '), '-'), ',' ORDER BY relname)
      FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname IN ('r1', 'c1', 'h1');" \
   "c1:ao_column:-,h1:heap:-,r1:ao_row:-"
refused "orientation without appendonly is Cloudberry's error" \
        "CREATE TABLE bad1 (a int) WITH (orientation=column);" \
        "invalid option \"orientation\" for base relation"
is "USING, where a statement has both, before the options, as in Cloudberry" \
   "CREATE TABLE both1 (a int) USING heap WITH (appendonly=true);
    SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 'both1';" "heap"
q "CREATE TABLE opts (a int) WITH (appendonly=true, compresstype=zlib, compresslevel=5, blocksize=65536, checksum=false);" > /dev/null
is "the method's options are kept in reloptions, and read back resolved" \
   "SELECT array_to_string(reloptions, ' ') || ' / ' ||
           (SELECT blocksize || ' ' || compresstype || ' ' || compresslevel || ' ' || checksum FROM gp_ao.options('opts'))
      FROM pg_class WHERE relname = 'opts';" \
   "compresstype=zlib compresslevel=5 blocksize=65536 checksum=false / 65536 zlib 5 false"
is "pg_appendonly shows them as Cloudberry's catalog does" \
   "SELECT blocksize || ' ' || compresstype || ' ' || compresslevel || ' ' || checksum || ' ' || columnstore
      FROM pg_appendonly WHERE relid = 'opts'::regclass;" "65536 zlib 5 false false"
refused "a block size that is no multiple of 8K is refused" \
        "CREATE TABLE bad3 (a int) WITH (appendonly=true, blocksize=10000);" "8KB multiple"
refused "and a compression type there is none of" \
        "CREATE TABLE bad4 (a int) WITH (appendonly=true, compresstype=lz9);" "unknown compresstype"
refused "rle_type is for a table by column" \
        "CREATE TABLE bad5 (a int) WITH (appendonly=true, compresstype=rle_type);" \
        "rle_type cannot be used with Append Only relations row orientation"
refused "an option heap has not either" \
        "CREATE TABLE bad6 (a int) WITH (appendonly=true, nonsense=1);" "unrecognized parameter \"nonsense\""
refused "an unlogged one is refused: its pages would have no WAL to be rebuilt from" \
        "CREATE UNLOGGED TABLE bad7 (a int) USING ao_row;" "unlogged append-optimized tables are not supported"

###############################################################################
echo "2. rows are appended in blocks, in the table's own pages"
###############################################################################
q "INSERT INTO r1 SELECT i, 'row ' || i FROM generate_series(1, 5000) i;
   INSERT INTO c1 SELECT i, 'row ' || i FROM generate_series(1, 5000) i;" > /dev/null
is "what goes in comes out, by row" \
   "SELECT count(*) || ' ' || sum(a) || ' ' || min(b) || ' ' || max(b) FROM r1;" "5000 12502500 row 1 row 999"
is "and by column" \
   "SELECT count(*) || ' ' || sum(a) || ' ' || min(b) || ' ' || max(b) FROM c1;" "5000 12502500 row 1 row 999"
is "a table's first page says which storage it is" \
   "SELECT gp_ao.storage_id('r1') IS NOT NULL AND gp_ao.storage_id('r1') <> gp_ao.storage_id('c1');" "t"
is "one segment file for one writer, its end and rows in gp_ao.segfile" \
   "SELECT segno || ' ' || tupcount || ' ' || (eof[1] > 0) || ' ' || state
      FROM gp_ao.segfile WHERE storage_id = gp_ao.storage_id('r1');" "1 5000 true 1"
is "a table by column has a file a column" \
   "SELECT array_length(eof, 1) FROM gp_ao.segfile WHERE storage_id = gp_ao.storage_id('c1');" "2"
is "the block directory lists every row once" \
   "SELECT sum(nrows) = 5000 AND min(first_row) = 1 FROM gp_ao.blkdir WHERE storage_id = gp_ao.storage_id('r1');" "t"
is "a TID is the segment file in the high 7 bits of the block, and the row number" \
   "SELECT string_agg(ctid::text, ' ' ORDER BY a) FROM r1 WHERE a IN (1, 290, 291, 5000);" \
   "(33554432,2) (33554432,291) (33554433,1) (33554449,54)"
is "the relation's size is its pages', a multiple of 8K" \
   "SELECT pg_relation_size('r1') > 0 AND pg_relation_size('r1') % 8192 = 0;" "t"
q "CREATE TABLE nulls (a int, b text, c numeric, d int[]) USING ao_column;
   INSERT INTO nulls VALUES (1, NULL, 1.5, '{1,2}'), (NULL, 'x', NULL, NULL), (3, 'y', 2.5, '{}');" > /dev/null
is "NULLs and every kind of value, by column" \
   "SELECT string_agg(coalesce(a::text, '-') || coalesce(b, '-') || coalesce(c::text, '-') || coalesce(d::text, '-'), ' ' ORDER BY a NULLS FIRST) FROM nulls;" \
   "-x-- 1-1.5{1,2} 3y2.5{}"
q "CREATE TABLE big (a int, b text) USING ao_row;
   INSERT INTO big SELECT i, repeat(md5(i::text), 50000) FROM generate_series(1, 4) i;" > /dev/null
is "a value longer than a block is kept whole, with no TOAST table" \
   "SELECT sum(length(b)) || ' ' || (SELECT reltoastrelid FROM pg_class WHERE relname = 'big') FROM big;" "6400000 0"

###############################################################################
echo "3. compression"
###############################################################################
for t in zlib zstd; do
	q "CREATE TABLE z_$t (a int, b text) WITH (appendonly=true, orientation=column, compresstype=$t);
	   INSERT INTO z_$t SELECT i, repeat('abc', 100) FROM generate_series(1, 5000) i;" > /dev/null
done
q "CREATE TABLE z_rle (a int, b text) WITH (appendonly=true, orientation=column, compresstype=rle_type);
   INSERT INTO z_rle SELECT i / 100, 'same' FROM generate_series(1, 5000) i;
   CREATE TABLE z_none (a int, b text) WITH (appendonly=true, orientation=column);
   INSERT INTO z_none SELECT i, repeat('abc', 100) FROM generate_series(1, 5000) i;" > /dev/null
is "zlib, zstd and rle_type read back what was written" \
   "SELECT (SELECT sum(length(b)) FROM z_zlib) || ' ' || (SELECT sum(length(b)) FROM z_zstd) || ' ' ||
           (SELECT sum(a) FROM z_rle);" "1500000 1500000 122550"
is "and take a fraction of the pages" \
   "SELECT pg_relation_size('z_zlib') * 4 < pg_relation_size('z_none') AND
           pg_relation_size('z_zstd') * 4 < pg_relation_size('z_none');" "t"
is "get_ao_compression_ratio() says how much, as Cloudberry's does" \
   "SELECT get_ao_compression_ratio('z_zlib') > 4 AND get_ao_compression_ratio('z_none') = 1;" "t"

###############################################################################
echo "4. indexes find rows by the block directory"
###############################################################################
q "CREATE INDEX r1_a ON r1 (a); CREATE INDEX c1_a ON c1 (a);" > /dev/null
isl "an index scan" \
   "SET enable_seqscan = off; SET enable_bitmapscan = off;
    SELECT string_agg(b, ',' ORDER BY a) FROM r1 WHERE a BETWEEN 290 AND 292;" "row 290,row 291,row 292"
isl "a bitmap scan" \
   "SET enable_seqscan = off; SET enable_indexscan = off;
    SELECT count(*) FROM c1 WHERE a < 1000;" "999"
isl "an index-only scan, which has no visibility map and reads the rows" \
   "SET enable_seqscan = off; SET enable_bitmapscan = off;
    EXPLAIN (COSTS OFF) SELECT a FROM c1 WHERE a = 7;" "  Index Cond: (a = 7)"
q "CREATE TABLE u (id int PRIMARY KEY, v text) USING ao_row;
   INSERT INTO u SELECT i, 'v' FROM generate_series(1, 100) i;" > /dev/null
refused "a unique index refuses a key the table has (O16)" \
        "INSERT INTO u VALUES (5, 'again');" "duplicate key value violates unique constraint"
refused "and one the same statement wrote, in a block not yet written" \
        "INSERT INTO u VALUES (500, 'a'), (500, 'b');" "duplicate key value violates unique constraint"
isl "an UPDATE of the key moves the row, as a heap's would" \
   "UPDATE u SET id = id + 1000 WHERE id <= 2; SELECT string_agg(id::text, ',' ORDER BY id) FROM u WHERE id > 100;" "1001,1002"
q "CREATE TABLE br (a int, b text) USING ao_row;
   INSERT INTO br SELECT i, 'x' FROM generate_series(1, 20000) i;
   CREATE INDEX br_a ON br USING brin (a) WITH (pages_per_range = 4);" > /dev/null
isl "BRIN walks each segment file's run of blocks (O18)" \
   "SET enable_seqscan = off; SELECT count(*) FROM br WHERE a BETWEEN 100 AND 200;" "101"
is "and summarizes the ones a later insert adds" \
   "INSERT INTO br SELECT i, 'y' FROM generate_series(20001, 30000) i;
    SELECT brin_summarize_new_values('br_a') > 0;" "t"

###############################################################################
echo "5. DELETE marks the visibility map; UPDATE fetches the old row by its TID"
###############################################################################
is "a DELETE" "DELETE FROM r1 WHERE a % 10 = 0; SELECT count(*) FROM r1;" "4500"
is "each deleted row is a bit of the map" \
   "SELECT count(*) FROM gp_toolkit.__gp_aovisimap('r1');" "500"
is "an UPDATE deletes the old version and appends the new" \
   "UPDATE r1 SET b = 'updated' WHERE a < 20; SELECT count(*) || ' ' || count(*) FILTER (WHERE b = 'updated') FROM r1;" "4500 18"
is "an UPDATE whose join reaches a row twice updates it once" \
   "UPDATE r1 SET b = 'twice' FROM (VALUES (1), (1)) v(x) WHERE r1.a = v.x;
    SELECT count(*) FROM r1 WHERE a = 1;" "1"
is "RETURNING gives the new rows" \
   "UPDATE c1 SET b = 'c' WHERE a = 3 RETURNING a, b;" "3|c"
# The old row keeps its table and the CTID a scan gives it, which the fetch
# by TID gives ao_row's slot, a minimal tuple's, and ao_column's, a virtual
# one.
is "DELETE ... RETURNING tableoid, ctid gives the row's table and CTID" \
   "CREATE TEMP TABLE old_r AS SELECT ctid AS c FROM r1 WHERE a = 21;
    WITH d AS (DELETE FROM r1 WHERE a = 21 RETURNING tableoid::regclass AS t, ctid AS c)
    SELECT t || ':' || (d.c = old_r.c) FROM d, old_r;" "r1:true"
is "and so does UPDATE's RETURNING old.tableoid, old.ctid, of a column table" \
   "CREATE TEMP TABLE old_c AS SELECT ctid AS c FROM c1 WHERE a = 6;
    WITH u AS (UPDATE c1 SET b = b WHERE a = 6 RETURNING old.tableoid::regclass AS t, old.ctid AS c)
    SELECT t || ':' || (u.c = old_c.c) FROM u, old_c;" "c1:true"
is "MERGE updates, deletes and inserts" \
   "MERGE INTO c1 USING (VALUES (4, 'u'), (5, 'd'), (9999, 'i')) s(a, b) ON c1.a = s.a
      WHEN MATCHED AND s.b = 'd' THEN DELETE
      WHEN MATCHED THEN UPDATE SET b = s.b
      WHEN NOT MATCHED THEN INSERT VALUES (s.a, s.b);
    SELECT string_agg(a || b, ',' ORDER BY a) FROM c1 WHERE a IN (4, 5, 9999);" "4u,9999i"
is "an AFTER INSERT row trigger finds its rows, which COPY fires before it ends the insert" \
   "CREATE TABLE trg_t (a int) USING ao_row; CREATE TABLE trg_log (a int);
    CREATE FUNCTION trg_ins() RETURNS trigger LANGUAGE plpgsql AS
      'BEGIN INSERT INTO trg_log VALUES (NEW.a); RETURN NEW; END';
    CREATE TRIGGER ti AFTER INSERT ON trg_t FOR EACH ROW EXECUTE FUNCTION trg_ins();
    COPY trg_t FROM PROGRAM 'seq 1 3'; INSERT INTO trg_t VALUES (4);
    SELECT string_agg(a::text, ',' ORDER BY a) FROM trg_log;" "1,2,3,4"
refused "row triggers on UPDATE are Cloudberry's error" \
        "CREATE FUNCTION trg() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
         CREATE TRIGGER t BEFORE UPDATE ON r1 FOR EACH ROW EXECUTE FUNCTION trg();" \
        "ON UPDATE triggers are not supported on append-only tables"
refused "and WHERE CURRENT OF" \
        "BEGIN; DECLARE cur CURSOR FOR SELECT * FROM r1; FETCH 1 FROM cur;
         UPDATE r1 SET b = 'x' WHERE CURRENT OF cur;" "\"r1\" is not simply updatable"
refused "ON CONFLICT is refused" \
        "INSERT INTO u VALUES (1, 'x') ON CONFLICT DO NOTHING;" "INSERT ON CONFLICT is not supported for appendoptimized relations"
is "gp.select_invisible shows the rows deleted, as gp_select_invisible does" \
   "SET gp.select_invisible = on; SELECT count(*) FROM r1;" "5019"

# Each old row is fetched by its TID, and the fetch keeps its descriptor, and
# the block it decoded, for the query's next row (ao_am.c): over two segment
# files of many blocks, by row and by column, compressed; a unique index's
# placeholders; rows reached out of the table's order; a row a function the
# statement calls deletes, or inserts, meanwhile; a TID scan's rows, with
# the query's snapshot; and an AFTER INSERT trigger's, which COPY fetches
# before it ends its insert.
session 3 8
q "CREATE TABLE fr (a int, b int, c text) USING ao_row WITH (compresstype=zstd);
   CREATE TABLE fc (a int, b int, c text) USING ao_column WITH (compresstype=zlib);" > /dev/null
echo "BEGIN; INSERT INTO fr SELECT i, i, 'r' || i FROM generate_series(1, 10000) i;
      INSERT INTO fc SELECT i, i, 'c' || i FROM generate_series(1, 10000) i;" >&8
settled s3 || notok "the third session began"
q "INSERT INTO fr SELECT i, i, 'r' || i FROM generate_series(10001, 20000) i;
   INSERT INTO fc SELECT i, i, 'c' || i FROM generate_series(10001, 20000) i;" > /dev/null
echo "COMMIT;" >&8
settled s3
is "two segment files of many blocks each, by row and by column" \
   "SELECT count(*) || ' ' || min(n) FROM (SELECT count(*) AS n FROM gp_ao.blkdir
      WHERE storage_id IN (gp_ao.storage_id('fr'), gp_ao.storage_id('fc'))
      GROUP BY storage_id, segno) s;" "4 2"
is "an UPDATE of every row fetches each by its TID, and updates it once" \
   "UPDATE fr SET b = b + 1; UPDATE fc SET b = b + 1;
    SELECT (SELECT count(*) || ':' || sum(b) || ':' || sum(length(c)) FROM fr) || ' ' ||
           (SELECT count(*) || ':' || sum(b) || ':' || sum(length(c)) FROM fc);" \
   "20000:200030000:108894 20000:200030000:108894"
is "and its RETURNING reads the old row each new one is built over" \
   "WITH u AS (UPDATE fc SET b = b * 2 RETURNING old.b AS o, new.b AS n, old.c = new.c AS same)
    SELECT count(*) || ' ' || sum(n - 2 * o) || ' ' || count(*) FILTER (WHERE same) FROM u;" "20000 0 20000"
q "CREATE INDEX fr_a ON fr (a);" > /dev/null
isl "an UPDATE that reaches the rows out of the table's order, through an index" \
   "SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_seqscan = off;
    SET enable_bitmapscan = off;
    UPDATE fr SET b = fr.b + 1
      FROM (SELECT g FROM generate_series(1, 20000) g ORDER BY md5(g::text)) s WHERE fr.a = s.g;
    SELECT count(*) || ' ' || sum(b) FROM fr;" "20000 200050000"
is "MERGE's matched rows" \
   "MERGE INTO fr USING (SELECT g FROM generate_series(1, 20000, 2) g) s ON fr.a = s.g
      WHEN MATCHED THEN UPDATE SET b = fr.b + 1000;
    SELECT count(*) || ' ' || sum(b) FROM fr;" "20000 210050000"
is "DELETE ... RETURNING of every third row" \
   "WITH d AS (DELETE FROM fc WHERE a % 3 = 0 RETURNING a, c, tableoid::regclass AS t)
    SELECT count(*) || ' ' || sum(a) || ' ' || sum(length(c)) || ' ' || min(t::text) FROM d;" \
   "6666 66663333 36294 fc"
is "and the rows it leaves" "SELECT count(*) || ' ' || sum(a) FROM fc;" "13334 133346667"
q "CREATE TABLE fu (id int PRIMARY KEY, v int, w text) USING ao_column;
   INSERT INTO fu SELECT i, i, 'w' || i FROM generate_series(1, 20000) i;" > /dev/null
isl "an UPDATE of a table with a unique index, whose new blocks each have a placeholder" \
   "UPDATE fu SET v = v + 1;
    SET enable_seqscan = off; SET enable_bitmapscan = off;
    SELECT count(*) || ' ' || sum(v) || ' ' || count(DISTINCT w) FROM fu WHERE id BETWEEN 1 AND 20000;" \
   "20000 200030000 20000"
q "CREATE TABLE fd (a int, b int) USING ao_row;
   INSERT INTO fd SELECT i, i FROM generate_series(1, 1000) i;
   CREATE FUNCTION fd_del(x int) RETURNS int LANGUAGE plpgsql AS \$\$
     BEGIN IF x = 10 THEN DELETE FROM fd WHERE a = 500; END IF; RETURN x; END \$\$;
   CREATE FUNCTION fd_ins(x int) RETURNS int LANGUAGE plpgsql AS \$\$
     BEGIN INSERT INTO fd VALUES (x + 1000, 0); RETURN x + 1; END \$\$;" > /dev/null
refused "a row a function the UPDATE calls deleted first is not found, as the map says now" \
        "UPDATE fd SET b = fd_del(a);" "failed to fetch tuple being updated"
is "and rows one inserts are not reached, the old ones each updated once" \
   "UPDATE fd SET b = fd_ins(a); SELECT count(*) || ' ' || sum(b) FROM fd;" "2000 501500"
is "a TID scan fetches each row it names, with the query's snapshot" \
   "SELECT count(*) || ' ' || sum(a) FROM fr WHERE ctid = ANY (ARRAY(SELECT ctid FROM fr WHERE a % 7 = 0));" \
   "2857 28578571"
is "an AFTER INSERT trigger's rows, 5000 of a COPY, each fetched before the insert ends" \
   "TRUNCATE trg_t, trg_log; COPY trg_t FROM PROGRAM 'seq 1 5000';
    SELECT count(*) || ' ' || sum(a) FROM trg_log;" "5000 12502500"

###############################################################################
echo "6. transactions: segment files, row numbers and savepoints"
###############################################################################
q "CREATE TABLE tx (a int) USING ao_row;" > /dev/null
q "BEGIN; INSERT INTO tx SELECT generate_series(1, 10); ROLLBACK;" > /dev/null
is "a rolled back insert leaves nothing" "SELECT count(*) FROM tx;" "0"
is "and the row numbers it used are never handed out again: an index may name them" \
   "INSERT INTO tx VALUES (1); SELECT ctid FROM tx;" "(33554432,12)"
isl "a savepoint rolled back takes its rows alone" \
   "BEGIN; INSERT INTO tx VALUES (2); SAVEPOINT s; INSERT INTO tx VALUES (3); ROLLBACK TO s;
    INSERT INTO tx VALUES (4); SAVEPOINT t; INSERT INTO tx VALUES (5); RELEASE t; COMMIT;
    SELECT string_agg(a::text, ',' ORDER BY a) FROM tx;" "1,2,4,5"
isl "an exception block's insert rolls back and the statement's own rows stay" \
   "CREATE FUNCTION ins_fail(x int) RETURNS int LANGUAGE plpgsql AS \$\$
      BEGIN
        BEGIN INSERT INTO tx VALUES (x * 100); PERFORM 1 / 0;
        EXCEPTION WHEN division_by_zero THEN NULL; END;
        RETURN x;
      END \$\$;
    INSERT INTO tx SELECT ins_fail(i) FROM generate_series(10, 12) i;
    SELECT string_agg(a::text, ',' ORDER BY a) FROM tx;" "1,2,4,5,10,11,12"
isl "rows a function inserts through a query of its own are its query's, and seen after it" \
   "CREATE FUNCTION ins_count(x int) RETURNS bigint LANGUAGE plpgsql AS \$\$
      BEGIN INSERT INTO tx VALUES (x); RETURN (SELECT count(*) FROM tx WHERE a = x); END \$\$;
    SELECT string_agg(ins_count(i)::text, ',') FROM generate_series(20, 22) i;" "1,1,1"

session 2 7
echo "BEGIN; INSERT INTO tx VALUES (1000);" >&7
settled s2 || notok "the second session began"
q "INSERT INTO tx VALUES (2000);" > /dev/null
is "two writers at once write two segment files" \
   "SELECT count(*) FROM gp_ao.segfile WHERE storage_id = gp_ao.storage_id('tx');" "2"
echo "COMMIT;" >&7
settled s2
is "and both are read" "SELECT count(*) FROM tx WHERE a IN (1000, 2000);" "2"

echo "BEGIN; INSERT INTO u VALUES (3000, 'first');" >&7
settled s2
q "INSERT INTO u VALUES (3000, 'second');" > "$WORK/u.out" 2>&1 &
waiter=$!
waits "INSERT INTO u VALUES (3000" \
	&& ok "a unique probe that meets a writer's block waits for it, as a heap's waits for a row" \
	|| notok "a unique probe waits for the writer" "$(q "SELECT state, wait_event_type, query FROM pg_stat_activity WHERE query LIKE 'INSERT INTO u%';")"
echo "ROLLBACK;" >&7
settled s2
wait "$waiter"
is "and goes ahead when the writer rolls back" "SELECT v FROM u WHERE id = 3000;" "second"

###############################################################################
echo "7. VACUUM compacts, and recycles what no snapshot sees any more"
###############################################################################
q "CREATE TABLE vc (a int, b text) USING ao_column;
   CREATE INDEX vc_a ON vc (a);
   INSERT INTO vc SELECT i, 'x' FROM generate_series(1, 10000) i;
   DELETE FROM vc WHERE a <= 6000;" > /dev/null
echo "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM vc;" >&7
settled s2
q "VACUUM vc;" > /dev/null
is "VACUUM moved the live rows to another segment file" \
   "SELECT string_agg(segno || ':' || tupcount || ':' || state, ' ' ORDER BY segno)
      FROM gp_ao.segfile WHERE storage_id = gp_ao.storage_id('vc');" "1:10000:2 2:4000:1"
is "a new snapshot reads each row once, by the table and by the index" \
   "SELECT count(*) FROM vc;
    SET enable_seqscan = off; SET enable_bitmapscan = off;
    SELECT count(*) FROM vc WHERE a > 5990 AND a < 6010;" "4000
9"
echo "SELECT count(*) FROM vc;" >&7
settled s2
[ "$(tail -1 "$WORK/s2.out")" = 4000 ] && ok "and one older than the VACUUM still reads the old file" \
	|| notok "an older snapshot reads the old file" "$(tail -3 "$WORK/s2.out")"
echo "COMMIT;" >&7
settled s2
q "VACUUM vc;" > /dev/null
is "the next VACUUM, with no snapshot older than the first, recycles the file" \
   "SELECT string_agg(segno || ':' || tupcount || ':' || state, ' ' ORDER BY segno)
      FROM gp_ao.segfile WHERE storage_id = gp_ao.storage_id('vc');" "1:0:1 2:4000:1"
is "and its rows' index entries" \
   "SELECT count(*) FROM gp_ao.blkdir WHERE storage_id = gp_ao.storage_id('vc') AND segno = 1;" "0"
q "INSERT INTO vc SELECT i, 'y' FROM generate_series(1, 100) i;" > /dev/null
is "a recycled segment file is written again, after the row numbers it had" \
   "SELECT min(ctid) > '(33554432,1)'::tid FROM vc WHERE b = 'y';" "t"
q "CREATE TABLE vd (a int) USING ao_row; INSERT INTO vd SELECT generate_series(1, 1000);
   DELETE FROM vd;" > /dev/null
q "VACUUM vd;" > /dev/null
is "VACUUM ends by recycling what it compacted, when nothing older runs beside it" \
   "SELECT string_agg(segno || ':' || tupcount || ':' || state, ' ' ORDER BY segno)
      FROM gp_ao.segfile WHERE storage_id = gp_ao.storage_id('vd');" "1:0:1"
q "INSERT INTO vd SELECT generate_series(1, 10); DELETE FROM vd;" > /dev/null
"$PSQL" -X -q -d postgres -c "SET gp.appendonly_compaction = off" -c "VACUUM vd" > /dev/null 2>&1
is "gp.appendonly_compaction turns compaction off" \
   "SELECT sum(tupcount) FROM gp_ao.segfile WHERE storage_id = gp_ao.storage_id('vd');" "10"
is "VACUUM counts the live rows for the planner" \
   "SELECT reltuples FROM pg_class WHERE relname = 'vc';" "4000"

###############################################################################
echo "8. DDL: new files are a new storage ID, and the old ones' rows go with them"
###############################################################################
q "CREATE TABLE dd (a int, b text) USING ao_column; INSERT INTO dd SELECT i, 'x' FROM generate_series(1, 100) i;" > /dev/null
SID=$(q "SELECT gp_ao.storage_id('dd');")
isl "TRUNCATE rolled back keeps the rows" \
   "BEGIN; TRUNCATE dd; ROLLBACK; SELECT count(*) FROM dd;" "100"
isl "TRUNCATE takes a new storage ID, and the old one's rows go" \
   "TRUNCATE dd; SELECT count(*) || ' ' || (gp_ao.storage_id('dd') <> $SID) || ' ' ||
           (SELECT count(*) FROM gp_ao.segfile WHERE storage_id = $SID) FROM dd;" "0 true 0"
q "INSERT INTO dd SELECT i, 'x' FROM generate_series(1, 100) i;" > /dev/null
is "ADD COLUMN with a constant reads the old rows as having it" \
   "ALTER TABLE dd ADD COLUMN c int DEFAULT 7; SELECT sum(c) FROM dd;" "700"
RFN=$(q "SELECT relfilenode FROM pg_class WHERE relname = 'dd';")
is "ADD COLUMN with a volatile default writes the new column alone (O17)" \
   "ALTER TABLE dd ADD COLUMN v float8 DEFAULT random(), ADD COLUMN g int GENERATED ALWAYS AS (a * 2) STORED;
    SELECT count(v) || ' ' || (sum(g) = sum(a) * 2) || ' ' ||
           ((SELECT relfilenode FROM pg_class WHERE relname = 'dd') = $RFN) FROM dd;" "100 true true"
refused "and a constraint of it is checked against what was written" \
        "ALTER TABLE dd ADD COLUMN w int DEFAULT (random() * 0)::int CHECK (w > 0);" "is violated by some row"
q "ALTER TABLE dd DROP COLUMN v; ALTER TABLE dd DROP COLUMN g;" > /dev/null
is "ALTER COLUMN TYPE rewrites the table, reading the old values as their old type" \
   "ALTER TABLE dd ALTER COLUMN c TYPE numeric; ALTER TABLE dd ALTER COLUMN c TYPE int4;
    SELECT sum(c) || ' ' || pg_typeof(sum(c)) FROM dd;" "700 bigint"
q "ALTER TABLE dd DROP COLUMN b;" > /dev/null
q "VACUUM FULL dd;" > /dev/null
is "DROP COLUMN, and VACUUM FULL's rewrite after it" \
   "SELECT count(*) || ' ' || sum(c) FROM dd;" "100 700"
is "CLUSTER" "CREATE INDEX dd_a ON dd (a); CLUSTER dd USING dd_a; SELECT count(*) FROM dd;" "100"
mkdir -p "$WORK/ts"
q "CREATE TABLESPACE ao_ts LOCATION '$WORK/ts';" > /dev/null
SID=$(q "SELECT gp_ao.storage_id('dd');")
is "SET TABLESPACE copies the pages, and the storage ID with them" \
   "ALTER TABLE dd SET TABLESPACE ao_ts; SELECT count(*) || ' ' || (gp_ao.storage_id('dd') = $SID) FROM dd;" "100 true"
SID=$(q "SELECT gp_ao.storage_id('dd');")
is "DROP TABLE takes the rows of gp_ao's tables with it" \
   "DROP TABLE dd; SELECT count(*) FROM gp_ao.segfile WHERE storage_id = $SID;" "0"
# A tablespace outside the data directory would be the standby's to share.
q "DROP TABLESPACE ao_ts;" > /dev/null
q "CREATE TABLE am (a int, b text) WITH (fillfactor = 70);
   INSERT INTO am SELECT i, 'x' FROM generate_series(1, 100) i;" > /dev/null
is "SET WITH (appendonly=true, ...), Cloudberry's spelling, changes the method and the options" \
   "ALTER TABLE am SET WITH (appendonly = true, compresslevel = 3);
    SELECT amname || ' ' || array_to_string(reloptions, ' ') || ' ' || (SELECT count(*) FROM am)
      FROM pg_class c JOIN pg_am m ON m.oid = relam WHERE relname = 'am';" "ao_row compresslevel=3 100"
refused "and names a method, or is refused as Cloudberry's grammar refuses it" \
        "ALTER TABLE am SET WITH (compresslevel = 3);" "invalid storage type"
refused "SET ACCESS METHOD m WITH (...) with options naming another method" \
        "ALTER TABLE am SET ACCESS METHOD ao_row WITH (appendonly = true, orientation = column);" \
        "ACCESS METHOD is specified as \"ao_row\" but the WITH option indicates it to be \"ao_column\""
is "SET ACCESS METHOD ao_column WITH (...) gives every column its encoding" \
   "ALTER TABLE am SET ACCESS METHOD ao_column WITH (compresstype = zlib);
    SELECT array_to_string(reloptions, ' ') || ' ' ||
           (SELECT count(*) FROM pg_attribute_encoding WHERE attrelid = 'am'::regclass) || ' ' ||
           (SELECT count(*) FROM am)
      FROM pg_class WHERE relname = 'am';" "compresstype=zlib 2 100"
refused "an option heap does not take, for heap" \
        "ALTER TABLE am SET ACCESS METHOD heap WITH (blocksize = 65536);" "unrecognized parameter \"blocksize\""
is "and back to heap: the encodings and the options go" \
   "ALTER TABLE am SET ACCESS METHOD heap WITH (fillfactor = 50);
    SELECT array_to_string(reloptions, ' ') || ' ' ||
           (SELECT count(*) FROM pg_attribute_encoding WHERE attrelid = 'am'::regclass) || ' ' ||
           (SELECT count(*) FROM am)
      FROM pg_class WHERE relname = 'am';" "fillfactor=50 0 100"

###############################################################################
echo "9. ANALYZE, TABLESAMPLE and a scan of the columns a plan reads (O15)"
###############################################################################
is "ANALYZE samples every live row" \
   "ANALYZE c1; SELECT reltuples FROM pg_class WHERE relname = 'c1';" "5000"
is "TABLESAMPLE takes rows as Cloudberry's does" \
   "SELECT count(*) FROM c1 TABLESAMPLE SYSTEM (100);" "5000"
is "a scan by column reads the columns the plan names" \
   "SELECT count(*) FROM c1 WHERE a > 10;" "4991"

###############################################################################
echo "10. ENCODING, a partitioned table's options: labels, which pg_dump carries"
###############################################################################
q "CREATE TABLE enc (a int ENCODING (compresstype=zlib, compresslevel=5), b text,
                     COLUMN b ENCODING (blocksize=8192), c int,
                     DEFAULT COLUMN ENCODING (compresstype=zstd))
     WITH (appendonly=true, orientation=column);" > /dev/null
is "each column has its options, filled in as Cloudberry fills them" \
   "SELECT string_agg(attnum || ':' || array_to_string(attoptions, ' '), ', ' ORDER BY attnum)
      FROM pg_attribute_encoding WHERE attrelid = 'enc'::regclass;" \
   "1:compresstype=zlib compresslevel=5 blocksize=32768, 2:blocksize=8192 compresstype=none compresslevel=0, 3:compresstype=zstd compresslevel=1 blocksize=32768"
is "which are gp_ao's security labels of the columns" \
   "SELECT count(*) FROM pg_seclabel WHERE provider = 'gp_ao' AND objoid = 'enc'::regclass;" "3"
q "INSERT INTO enc SELECT i, repeat('x', 100), i % 7 FROM generate_series(1, 5000) i;" > /dev/null
is "and each column's blocks are compressed as its options say" \
   "SELECT string_agg(column_num || ':' || (eof < eof_uncompressed), ' ' ORDER BY column_num)
      FROM gp_toolkit.__gp_aocsseg('enc');" "0:true 1:false 2:true"
is "ALTER COLUMN SET ENCODING and ADD COLUMN ... ENCODING" \
   "ALTER TABLE enc ALTER COLUMN b SET ENCODING (compresstype=zlib),
                    ADD COLUMN d int ENCODING (compresstype=rle_type);
    SELECT string_agg(attnum || ':' || attoptions[1], ' ' ORDER BY attnum)
      FROM pg_attribute_encoding WHERE attrelid = 'enc'::regclass;" \
   "1:compresstype=zlib 2:compresstype=zlib 3:compresstype=zstd 4:compresstype=rle_type"
refused "ENCODING of a table by row is Cloudberry's error" \
        "CREATE TABLE enc_row (a int ENCODING (compresstype=zlib)) WITH (appendonly=true);" \
        "ENCODING clause only supported with column oriented tables"
q "CREATE TABLE ptab (a int, b text) PARTITION BY RANGE (a) WITH (appendonly=true, compresstype=zlib, compresslevel=2);
   CREATE TABLE ptab_1 PARTITION OF ptab FOR VALUES FROM (0) TO (10);
   CREATE TABLE ptab_2 PARTITION OF ptab FOR VALUES FROM (10) TO (20) WITH (compresslevel=7);
   CREATE TABLE ptab_3 PARTITION OF ptab FOR VALUES FROM (20) TO (30) WITH (appendonly=true, orientation=column);" > /dev/null
is "a partitioned table keeps its storage options for its partitions of its method" \
   "SELECT string_agg(relname || ':' || coalesce(array_to_string(reloptions, ' '), '-'), ', ' ORDER BY relname)
      FROM pg_class WHERE relname LIKE 'ptab%';" \
   "ptab:-, ptab_1:compresstype=zlib compresslevel=2, ptab_2:compresstype=zlib compresslevel=7, ptab_3:-"
is "a partition Cloudberry's partition clause makes takes its parent's options first" \
   "CREATE TABLE pc (a int, b int) WITH (appendonly=true, compresstype=zlib, compresslevel=5)
      PARTITION BY RANGE (b) (PARTITION p1 START (0) END (10) WITH (compresslevel=3),
                              DEFAULT PARTITION def);
    SELECT string_agg(relname || ':' || array_to_string(reloptions, ' '), ', ' ORDER BY relname)
      FROM pg_class WHERE relname LIKE 'pc_1_prt_%';" \
   "pc_1_prt_def:compresstype=zlib compresslevel=5, pc_1_prt_p1:compresstype=zlib compresslevel=3"
is "gp.default_storage_options fills in what a statement does not say" \
   "SET gp.default_storage_options = 'compresstype=zstd,blocksize=65536';
    CREATE TABLE dso (a int) USING ao_row;
    SELECT array_to_string(reloptions, ' ') FROM pg_class WHERE relname = 'dso';" \
   "blocksize=65536 compresstype=zstd"
is "and holds every option filled in, as SHOW prints Cloudberry's" \
   "SET gp.default_storage_options = 'compresslevel=3'; SHOW gp.default_storage_options;" \
   "blocksize=32768,compresstype=zlib,compresslevel=3,checksum=true"
refused "and refuses a method's option in Cloudberry's words" \
        "SET gp.default_storage_options = 'orientation=row';" "invalid storage option \"orientation\""
q "CREATE DATABASE restored;" > /dev/null
"$BINDIR/pg_dump" -d postgres -t enc -t 'ptab*' > "$WORK/enc.sql" 2> "$WORK/enc.err"
"$PSQL" -X -q -d restored -c "CREATE EXTENSION gp_ao CASCADE" > /dev/null 2>&1
"$PSQL" -X -q -d restored -f "$WORK/enc.sql" > "$WORK/enc.restore" 2>&1
qr() { "$PSQL" -X -q -t -A -d restored -c "$1" 2>&1; }
want=$(q "SELECT string_agg(attnum || ':' || array_to_string(attoptions, ' '), ', ' ORDER BY attnum) FROM pg_attribute_encoding WHERE attrelid = 'enc'::regclass;")
got=$(qr "SELECT string_agg(attnum || ':' || array_to_string(attoptions, ' '), ', ' ORDER BY attnum) FROM pg_attribute_encoding WHERE attrelid = 'enc'::regclass;")
[ -n "$want" ] && [ "$want" = "$got" ] && ok "pg_dump and a restore carry each column's options" \
	|| notok "pg_dump carries column options" "want [$want] got [$got] $(grep -i error "$WORK/enc.restore" | head -3)"
is "and the rows" "SELECT '$(qr "SELECT count(*) || ' ' || sum(d IS NULL::int) FROM enc;")';" "5000 5000"
got=$(qr "SELECT label FROM pg_seclabel WHERE objoid = 'ptab'::regclass AND provider = 'gp_ao';")
[ "$got" = "compresstype=zlib,compresslevel=2" ] && ok "and a partitioned table's options" \
	|| notok "pg_dump carries a partitioned table's options" "got [$got]"

###############################################################################
echo "11. the bitmap index, Cloudberry's, which gp_ao carries"
###############################################################################
is "an index access method, with a class for each of B-tree's in pg_catalog" \
   "SELECT count(*) > 30 FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod WHERE a.amname = 'bitmap';" "t"
q "CREATE TABLE bmh (a int, b text, c int);
   INSERT INTO bmh SELECT i, 'v' || (i % 10), i % 3 FROM generate_series(1, 50000) i;
   CREATE INDEX bmh_b ON bmh USING bitmap (b);
   CREATE INDEX bmh_c ON bmh USING bitmap (c);
   CREATE TABLE bma (a int, b int) USING ao_column;
   INSERT INTO bma SELECT i, i % 5 FROM generate_series(1, 50000) i;
   CREATE INDEX bma_b ON bma USING bitmap (b);" > /dev/null
is "its list of values is a heap and a B-tree of each index's, in pg_bitmapindex" \
   "SELECT count(*) FROM pg_class WHERE relnamespace = 'pg_bitmapindex'::regnamespace;" "6"
isl "a bitmap scan of a heap table" \
   "SET enable_seqscan = off; SELECT count(*) FROM bmh WHERE b = 'v3' AND c = 1;" "1667"
isl "an index scan" \
   "SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM bmh WHERE c = 2;" "16667"
isl "and of an append-optimized table, whose TIDs are the port's" \
   "SET enable_seqscan = off; SELECT count(*) FROM bma WHERE b = 2;" "10000"
q "INSERT INTO bmh SELECT i, 'new', 7 FROM generate_series(1, 100) i;
   DELETE FROM bma WHERE a <= 10000;" > /dev/null
q "VACUUM bmh;" > /dev/null
q "VACUUM bma;" > /dev/null
isl "rows inserted after the build are found, and VACUUM builds it again" \
   "SET enable_seqscan = off; SELECT (SELECT count(*) FROM bmh WHERE c = 7) || ' ' || (SELECT count(*) FROM bma WHERE b = 2);" "100 8000"
is "DROP INDEX drops its list of values" \
   "DROP INDEX bmh_b; SELECT count(*) FROM pg_class WHERE relnamespace = 'pg_bitmapindex'::regnamespace;" "4"
is "an UPDATE that breaks a bitmap's compressed words, and a page of them" \
   "CREATE TABLE bmw (i int, j int);
    CREATE INDEX bmw_i ON bmw USING bitmap (i);
    INSERT INTO bmw SELECT 1, CASE WHEN g % 264 = 0 THEN 2 ELSE 1 END FROM generate_series(1, 4096) g;
    UPDATE bmw SET i = 2 WHERE j = 2; UPDATE bmw SET i = 3 WHERE i = 1;
    SET enable_seqscan = off;
    SELECT (SELECT count(*) FROM bmw WHERE i = 1) || ' ' || (SELECT count(*) FROM bmw WHERE i = 2) || ' ' ||
           (SELECT count(*) FROM bmw WHERE i = 3);" "0 15 4081"

###############################################################################
echo "12. a standby replays gp_ao's records, and has the same rows"
###############################################################################
SB="$WORK/standby"
SBPORT=$((PORT + 1))
qs() { "$PSQL" -X -q -t -A -p "$SBPORT" -d postgres -c "$1" 2>&1; }
caught_up() {
	local lsn
	lsn=$(q "SELECT pg_current_wal_insert_lsn();")
	for _ in $(seq 150); do
		[ "$(qs "SELECT pg_last_wal_replay_lsn() >= '$lsn';")" = t ] && return 0
		sleep 0.2
	done
	return 1
}
"$BINDIR/pg_basebackup" -D "$SB" -X stream -c fast -R > "$WORK/basebackup.log" 2>&1 &&
	echo "port = $SBPORT" >> "$SB/postgresql.conf" &&
	"$BINDIR/pg_ctl" -D "$SB" -l "$WORK/standby.log" -w -t 60 start > /dev/null 2>&1
[ "$(qs "SELECT pg_is_in_recovery();")" = t ] && ok "a hot standby, streaming" \
	|| notok "a hot standby" "$(tail -n 3 "$WORK/basebackup.log" "$WORK/standby.log")"
q "CREATE TABLE rep (a int, b text) WITH (appendonly=true, orientation=column, compresstype=zlib);
   INSERT INTO rep SELECT i, md5(i::text) FROM generate_series(1, 50000) i;
   DELETE FROM rep WHERE a % 3 = 0;
   UPDATE rep SET b = 'u' WHERE a % 7 = 0;" > /dev/null
q "CREATE INDEX rep_b ON rep USING bitmap (b);
   INSERT INTO rep SELECT i, 'u' FROM generate_series(60001, 60100) i;" > /dev/null
caught_up || notok "the standby catches up"
want=$(q "SELECT count(*) || ' ' || md5(string_agg(a || b, ',' ORDER BY a)) FROM rep;")
is "the standby reads the rows the primary has" \
   "SELECT '$(qs "SELECT count(*) || ' ' || md5(string_agg(a || b, ',' ORDER BY a)) FROM rep;")';" "$want"
out=$(qs "INSERT INTO rep VALUES (1, 'x');")
case "$out" in *"read-only transaction"*) ok "and refuses to write any" ;; *) notok "the standby refuses to write" "$out" ;; esac
want=$(q "SET enable_seqscan = off; SELECT count(*) FROM rep WHERE b = 'u';" | tail -1)
got=$("$PSQL" -X -q -t -A -p "$SBPORT" -d postgres -c "SET enable_seqscan = off" -c "SELECT count(*) FROM rep WHERE b = 'u';" 2>&1 | tail -1)
[ -n "$want" ] && [ "$want" = "$got" ] && ok "its bitmap index, replayed through resource manager 201, answers as the primary's" \
	|| notok "the standby's bitmap index" "want [$want] got [$got]"
"$BINDIR/pg_ctl" -D "$SB" -m fast -w stop > /dev/null 2>&1

###############################################################################
echo "13. a crash: recovery replays the pages; without gp_ao it stops (check 12)"
###############################################################################
q "CHECKPOINT;
   INSERT INTO rep SELECT i, 'late' FROM generate_series(1, 1000) i;" > /dev/null
want=$(q "SELECT count(*) FROM rep;")
wantbm=$(q "SET enable_seqscan = off; SELECT count(*) FROM rep WHERE b = 'late';" | tail -1)
"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate -w stop > /dev/null 2>&1
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log-nogpao" -o "-c shared_preload_libraries=gp_core,gp_sql" \
	-w -t 60 start > /dev/null 2>&1
started=$?
out=$(grep -o 'FATAL:  resource manager with ID 200 not registered' "$WORK/log-nogpao" | head -1)
[ "$started" -ne 0 ] && [ -n "$out" ] \
	&& ok "without gp_ao preloaded, recovery stops at its first record, with PostgreSQL's FATAL" \
	|| notok "recovery without gp_ao" "started=$started $(grep -E 'FATAL|PANIC' "$WORK/log-nogpao" | head -3)"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1
is "with it, the rows written after the checkpoint are back" "SELECT count(*) FROM rep;" "$want"
isl "and the bitmap index finds them" \
   "SET enable_seqscan = off; SELECT count(*) FROM rep WHERE b = 'late';" "$wantbm"
# pg_waldump loads no extension, so it knows gp_ao's records by their ID.
if "$BINDIR/pg_waldump" -p "$WORK/data/pg_wal" -r custom200 \
	"$(ls "$WORK/data/pg_wal" | grep -E '^[0-9A-F]{24}$' | head -1)" 2>/dev/null | grep -q "custom200"; then
	ok "pg_waldump finds gp_ao's records, by their resource manager's ID"
else
	notok "pg_waldump finds gp_ao's records"
fi

###############################################################################
echo "14. pg_checksums verifies every page of an append-optimized table"
###############################################################################
q "CHECKPOINT;" > /dev/null
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w stop > /dev/null 2>&1
"$BINDIR/pg_checksums" --check -D "$WORK/data" > "$WORK/checksums.log" 2>&1 \
	&& ok "pg_checksums --check passes over them" \
	|| notok "pg_checksums --check" "$(tail -3 "$WORK/checksums.log")"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1
is "and the rows read back" "SELECT count(*) FROM rep;" "$want"

###############################################################################
echo "15. a PAX table's ENCODING clauses: gp_ao keeps them, PAX checks and writes by them"
###############################################################################
q "CREATE EXTENSION pax;" > /dev/null
q "CREATE TABLE px (a int ENCODING (compresstype=rle), b text, c int,
                    COLUMN c ENCODING (compresstype=zlib, compresslevel=3))
     USING pax WITH (compresstype=zstd, compresslevel=5);" > /dev/null
is "each column has what its statement says of it -- the table's own compression the default -- as given" \
   "SELECT string_agg(attnum || ':' || array_to_string(attoptions, ' '), ', ' ORDER BY attnum)
      FROM pg_attribute_encoding WHERE attrelid = 'px'::regclass;" \
   "1:compresstype=rle, 2:compresstype=zstd compresslevel=5, 3:compresstype=zlib compresslevel=3"
is "and a column of a table without them has none, PAX's defaults its own" \
   "CREATE TABLE pxn (a int, b int) USING pax;
    SELECT count(*) FROM pg_attribute_encoding WHERE attrelid = 'pxn'::regclass;" "0"
q "CREATE TABLE pxr (a int ENCODING (compresstype=rle), b int ENCODING (compresstype=zstd, compresslevel=19)) USING pax;
   INSERT INTO pxn SELECT i % 10, i % 100 FROM generate_series(1, 200000) i;
   INSERT INTO pxr SELECT i % 10, i % 100 FROM generate_series(1, 200000) i;
   INSERT INTO px SELECT i % 10, 'value ' || (i % 5), i FROM generate_series(1, 100000) i;" > /dev/null
is "which PAX writes each column by" \
   "SELECT pg_relation_size('pxr') * 5 < pg_relation_size('pxn');" "t"
is "and reads back" \
   "SELECT count(*) || ' ' || sum(a) || ' ' || sum(b) FROM pxr;" "200000 900000 9900000"
is "a table's too" \
   "SELECT count(*) || ' ' || sum(a) || ' ' || count(DISTINCT b) || ' ' || sum(c) FROM px;" \
   "100000 450000 5 5000050000"
refused "an encoding that is AO's, not PAX's, is PAX's error" \
        "CREATE TABLE pxbad (a int ENCODING (compresstype=rle_type)) USING pax;" \
        "unsupported compress type: 'rle_type'"
refused "and an option PAX takes from its table alone" \
        "CREATE TABLE pxbad (a int ENCODING (compresstype=zstd, blocksize=32768)) USING pax;" \
        "blocksize not allow setting in ENCODING CLAUSES."
refused "and a level an encoding has none of" \
        "CREATE TABLE pxbad (a int ENCODING (compresstype=rle, compresslevel=2)) USING pax;" \
        "compresslevel=2 should setting is not work for current encoding."
refused "a column named by two COLUMN ENCODING clauses is Cloudberry's error, for a table by column too" \
        "CREATE TABLE pxbad (a int, COLUMN a ENCODING (compresstype=zstd),
                             COLUMN a ENCODING (compresstype=zlib)) USING pax;" \
        "column \"a\" referenced in more than one COLUMN ENCODING clause"
is "ALTER COLUMN SET ENCODING and ADD COLUMN ... ENCODING, and ADD COLUMN given none" \
   "ALTER TABLE px ALTER COLUMN b SET ENCODING (compresstype=dict),
                   ADD COLUMN d int ENCODING (compresstype=zlib, compresslevel=1),
                   ADD COLUMN e int;
    SELECT string_agg(attnum || ':' || array_to_string(attoptions, ' '), ', ' ORDER BY attnum)
      FROM pg_attribute_encoding WHERE attrelid = 'px'::regclass;" \
   "1:compresstype=rle, 2:compresstype=dict, 3:compresstype=zlib compresslevel=3, 4:compresstype=zlib compresslevel=1"
refused "a label written by hand is PAX's to check" \
        "SECURITY LABEL FOR gp_ao ON COLUMN px.e IS 'compresstype=rle_type';" \
        "unsupported compress type: 'rle_type'"
is "a rewrite writes by them still" \
   "ALTER TABLE pxr ALTER COLUMN b TYPE bigint;
    SELECT (pg_relation_size('pxr') * 5 < pg_relation_size('pxn'))::text || ' ' || sum(b) FROM pxr;" \
   "true 9900000"
is "and a table that leaves PAX leaves them" \
   "ALTER TABLE pxr SET ACCESS METHOD heap;
    SELECT count(*) FROM pg_attribute_encoding WHERE attrelid = 'pxr'::regclass;" "0"
# Cloudberry's delta encoder takes a stream appended once, a text column's
# offsets; a column's values, appended one at a time, failed at a group's
# second, and a group of one could not be read back.  They are written as
# they are, the clause kept (pg19/pax/src/storage/columns/pax_encoding.cc).
is "a column ENCODING (compresstype=delta) takes groups of many rows, and reads them back" \
   "CREATE TABLE pxd (a int ENCODING (compresstype=delta), b int8, c date, d text)
      USING pax WITH (compresstype=delta);
    INSERT INTO pxd SELECT i, i, date '2020-01-01' + i % 1000, 'v' || i FROM generate_series(1, 200000) i;
    SELECT count(*) || ' ' || sum(a) || ' ' || sum(b) || ' ' || max(c) || ' ' || count(DISTINCT d) FROM pxd;" \
   "200000 20000100000 20000100000 2022-09-26 200000"

###############################################################################
echo "16. gp_toolkit's views of append-optimized tables, Cloudberry's"
###############################################################################
q "CREATE TABLE tkc (a int, b text) WITH (appendonly=true, orientation=column, compresstype=zlib);
   INSERT INTO tkc SELECT i % 10, repeat('abc', 50) FROM generate_series(1, 20000) i;
   CREATE TABLE tkp (a int, b int) PARTITION BY RANGE (a) WITH (appendonly=true);
   CREATE TABLE tkp_1 PARTITION OF tkp FOR VALUES FROM (0) TO (10);" > /dev/null
is "__gp_is_append_only: a table by column is, and a partition, not the partitioned table" \
   "SELECT string_agg(relname || ':' || iaotype, ' ' ORDER BY relname)
      FROM gp_toolkit.__gp_is_append_only JOIN pg_class ON oid = iaooid
     WHERE relname IN ('tkc', 'tkp', 'tkp_1');" "tkc:true tkp:false tkp_1:true"
is "gp_column_size_summary: each column's size, and its size uncompressed" \
   "SELECT string_agg(attname || ':' || (size < size_uncompressed)::text || ':' || (compression_ratio > 1)::text,
                      ' ' ORDER BY attnum)
      FROM gp_toolkit.gp_column_size_summary WHERE relname = 'tkc';" "a:true:true b:true:true"
is "gp_size_of_table_uncompressed, by the table's compression ratio" \
   "SELECT (sotu.sotusize > sotd.sotdsize)::text
      FROM gp_toolkit.gp_size_of_table_uncompressed sotu
      JOIN gp_toolkit.gp_size_of_table_disk sotd ON sotd.sotdoid = sotu.sotuoid
     WHERE sotu.sotutablename = 'tkc';" "true"
is "and gp_size_of_table_and_indexes_licensing beside it" \
   "SELECT (sotailtablesizeuncompressed > sotailtablesizedisk)::text
      FROM gp_toolkit.gp_size_of_table_and_indexes_licensing WHERE sotailtablename = 'tkc';" "true"

# The segment files' history: every version gp_ao.segfile holds, a dead
# one's too, as Cloudberry's reads its pg_aoseg under SnapshotAny -- here
# inside the transaction that wrote them, which no pruning reaches.
is "__gp_aoseg_history: each version of a segment file, as each write left it" \
   "BEGIN;
    CREATE TABLE tkh (a int, b text) WITH (appendonly=true);
    INSERT INTO tkh SELECT i, 'x' FROM generate_series(1, 100) i;
    INSERT INTO tkh SELECT i, 'y' FROM generate_series(1, 100) i;
    SELECT string_agg(segno || ':' || tupcount || ':' || modcount, ' ' ORDER BY tupcount)
      FROM gp_toolkit.__gp_aoseg_history('tkh');
    COMMIT;" "1:0:0 1:100:1 1:200:2"
is "__gp_aocsseg_history: each version, a row a column" \
   "BEGIN;
    CREATE TABLE tkhc (a int, b text) WITH (appendonly=true, orientation=column);
    INSERT INTO tkhc SELECT i, 'x' FROM generate_series(1, 100) i;
    SELECT string_agg(column_num || ':' || physical_segno || ':' || tupcount, ' '
                      ORDER BY column_num, tupcount)
      FROM gp_toolkit.__gp_aocsseg_history('tkhc');
    COMMIT;" "0:1:0 0:1:100 1:129:0 1:129:100"
refused "and a table by column's history as a row table's, refused in Cloudberry's words" \
   "SELECT * FROM gp_toolkit.__gp_aoseg_history('tkhc');" \
   "Relation 'tkhc' does not have appendoptimized row-oriented storage"

# The check for missing files with the files past a relation's first, which
# hold an append-optimized table's bytes past its first gigabyte: none
# here, and the table's first file is missed like any.
is "__get_ao_segno_list: no file past the first of a small table" \
   "SELECT count(*) FROM gp_toolkit.__get_ao_segno_list() UNION ALL
    SELECT count(*) FROM gp_toolkit.__get_aoco_segno_list();" "0
0"
q "CHECKPOINT;" > /dev/null
path=$(q "SELECT pg_relation_filepath('tkh');")
mv "$WORK/data/$path" "$WORK/tkh.file"
got=$(q "SELECT string_agg(relname, ' ') FROM gp_toolkit.__check_missing_files_ext;")
mv "$WORK/tkh.file" "$WORK/data/$path"
[ "$got" = "tkh" ] && ok "__check_missing_files_ext lists the table whose file is gone" \
	|| notok "__check_missing_files_ext" "want [tkh], got [$got]"

###############################################################################
echo "17. PAX's dump functions: a file's parts and data, printed as tables"
###############################################################################
# In pax.so, as Cloudberry builds them (storage/micro_partition_udf.cc), and
# created by hand as its source's comments say, since no script of PAX's
# creates them.  A PAX table's first file is <relation's path>_pax/0.
q "CREATE FUNCTION dump_pax_file_desc(file_path text, spcid oid) RETURNS text
     AS '\$libdir/pax', 'dump_pax_file_desc' LANGUAGE C IMMUTABLE;
   CREATE FUNCTION dump_pax_file_desc_schema(file_path text, spcid oid) RETURNS text
     AS '\$libdir/pax', 'dump_pax_file_desc_schema' LANGUAGE C IMMUTABLE;
   CREATE FUNCTION dump_pax_file_desc_group_info(file_path text, spcid oid, group_start int4, group_len int4) RETURNS text
     AS '\$libdir/pax', 'dump_pax_file_desc_group_info' LANGUAGE C IMMUTABLE;
   CREATE FUNCTION dump_pax_file_data(file_path text, toast_file_path text, spcid oid, groupid int4,
                                      colid_start int4, colid_len int4, rowid_start int4, rowid_len int4) RETURNS text
     AS '\$libdir/pax', 'dump_pax_file_data' LANGUAGE C IMMUTABLE;
   CREATE TABLE pxdump (a int, b int, c bigint) USING pax;
   INSERT INTO pxdump SELECT i, i * 10, i * 100 FROM generate_series(1, 5) i;" > /dev/null
desc=$(q "SELECT dump_pax_file_desc(pg_relation_filepath('pxdump') || '_pax/0', 0);")
case "$desc" in
	*"Magic         | PORC"*"Number Of Groups  | 1"*"Number Of Columns | 3"*"Number Of Rows    | 5"*)
		ok "dump_pax_file_desc(): the post script, the footer, the schema and each group of a file" ;;
	*) notok "dump_pax_file_desc()" "$desc" ;;
esac
data=$(q "SELECT dump_pax_file_data(pg_relation_filepath('pxdump') || '_pax/0', NULL, 0, 0, 0, 3, 0, 5);" |
	grep -E '^\| \| [0-9]' | tr -s ' ' | tr '\n' ' ')
[ "$data" = "$(for i in 1 2 3 4 5; do printf '| | %d | %d0 | %d00 | | ' $i $i $i; done)" ] \
	&& ok "dump_pax_file_data(): a group's rows, column by column" \
	|| notok "dump_pax_file_data()" "$data"
refused "and a file that is not there is refused" \
        "SELECT dump_pax_file_desc_schema('base/1/no_such_file', 0);" "Fail to open"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
