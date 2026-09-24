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
# Directory tables: files on disk with a row each.
#
# Cloudberry gives one a relkind of its own, a fixed schema, a catalog row
# saying where the files are, and a check in the executor.  Here it is an
# ordinary table, a label and an ExecutorStart hook, so what these tests ask
# is whether the same things are refused and the same things work -- and
# whether a file and its row stay in step when a transaction rolls back.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/dirtable/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-dir-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbf-XXXXXX)"
PORT="${PGPORT:-$((6900 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK"

pass=0; fail=0
ok()    { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok() { printf '  NOT OK %s\n' "$1"
          [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/         /'
          fail=$((fail + 1)); }

cleanup() {
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

echo "directory tables: an ordinary table, a label and files on disk"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "shared_preload_libraries = 'gp_core,gp_sql'"
} >> "$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_sql CASCADE;")
case "$out" in
	*ERROR*) echo "the extension could not be created:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. it is an ordinary table with a directory of its own"
###############################################################################
q "SELECT gp_sql.create_directory_table('docs');" > /dev/null
is "the relkind is a plain table, not one of Cloudberry's own" \
   "SELECT relkind FROM pg_class WHERE relname = 'docs';" "r"
is "the columns are Cloudberry's, in its order" \
   "SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
     WHERE attrelid = 'docs'::regclass AND attnum > 0 AND NOT attisdropped;" \
   "relative_path,size,last_modified,md5,tag"
is "the location reads back, which is what says it is one" \
   "SELECT gp_sql.directory_table_location('docs'::regclass) LIKE 'base/%_dirtable';" "t"
is "an ordinary table has none" \
   "CREATE TABLE plain (id int);
    SELECT gp_sql.directory_table_location('plain'::regclass) IS NULL;" "t"
LOC=$(q "SELECT gp_sql.directory_table_location('docs'::regclass);")
[ -d "$WORK/data/$LOC" ] && ok "and the directory exists on disk" \
	|| notok "the directory exists on disk" "$WORK/data/$LOC"
is "it is listed the way Cloudberry lists it" \
   "SELECT tablename FROM gp_sql.directory_tables;" "docs"

###############################################################################
echo "2. a file and its row are written together"
###############################################################################
is "put reports what it wrote" \
   "SELECT gp_sql.directory_table_put('docs'::regclass, 'a/b.txt', 'hello'::bytea, 'greeting');" "5"
is "the row describes the file" \
   "SELECT relative_path || ' ' || size || ' ' || md5 || ' ' || tag FROM docs;" \
   "a/b.txt 5 $(printf hello | md5sum | cut -d' ' -f1) greeting"
[ -f "$WORK/data/$LOC/a/b.txt" ] && ok "the file is there, in a directory it made" \
	|| notok "the file is there" "$(ls -R "$WORK/data/$LOC")"
is "and it can be read back" \
   "SELECT convert_from(gp_sql.directory_table_get('docs'::regclass, 'a/b.txt'), 'UTF8');" "hello"
is "a file that is not there reads as NULL" \
   "SELECT gp_sql.directory_table_get('docs'::regclass, 'nope') IS NULL;" "t"
is "directory_table() gives what Cloudberry's gives" \
   "SELECT relative_path || ' ' || tag || ' ' || size || ' ' || convert_from(content, 'UTF8')
      FROM gp_sql.directory_table('docs'::regclass);" \
   "a/b.txt greeting 5 hello"
is "and its scoped_file_url is where the file really is" \
   "SELECT scoped_file_url = gp_sql.directory_table_location('docs'::regclass) || '/a/b.txt'
      FROM gp_sql.directory_table('docs'::regclass);" "t"

###############################################################################
echo "3. the rows are written by these functions and by nothing else"
###############################################################################
refused "INSERT is refused" \
        "INSERT INTO docs VALUES ('x', 1, now(), 'm', 't');" \
        "cannot change directory table"
refused "DELETE is refused" "DELETE FROM docs;" "cannot change directory table"
refused "and so is UPDATE of a column that describes the file" \
        "UPDATE docs SET size = 0;" "only the \"tag\" column"
isl "but the tag may be updated, as in Cloudberry" \
   "UPDATE docs SET tag = 'note';
    SELECT tag FROM docs;" "note"
refused "TRUNCATE would orphan every file, so it is refused" \
        "TRUNCATE docs;" "cannot truncate directory table"
isl "a superuser can still turn the guard off" \
   "SET gp.allow_dml_directory_table = on;
    INSERT INTO docs VALUES ('ghost', 0, now(), '', '');
    SELECT count(*) FROM docs;" "2"
# A new session, so the setting is back to its default.
refused "and the next session is guarded again" \
        "DELETE FROM docs;" "cannot change directory table"
isl "the row written while it was off can be taken back the same way" \
   "SET gp.allow_dml_directory_table = on;
    DELETE FROM docs WHERE relative_path = 'ghost';
    SELECT count(*) FROM docs;" "1"

###############################################################################
echo "4. a file and its row roll back together"
###############################################################################
q "BEGIN; SELECT gp_sql.directory_table_put('docs'::regclass, 'rolled.txt', 'x'::bytea); ROLLBACK;" > /dev/null
is "a put that rolled back left no row" \
   "SELECT count(*) FROM docs WHERE relative_path = 'rolled.txt';" "0"
[ ! -f "$WORK/data/$LOC/rolled.txt" ] && ok "and no file" \
	|| notok "and no file" "$WORK/data/$LOC/rolled.txt is still there"
q "BEGIN; SELECT gp_sql.remove_file('docs'::regclass, 'a/b.txt'); ROLLBACK;" > /dev/null
is "a removal that rolled back kept the row" \
   "SELECT count(*) FROM docs WHERE relative_path = 'a/b.txt';" "1"
[ -f "$WORK/data/$LOC/a/b.txt" ] && ok "and the file" \
	|| notok "and the file" "$WORK/data/$LOC/a/b.txt is gone"
is "remove_file says whether it removed anything" \
   "SELECT gp_sql.remove_file('docs'::regclass, 'a/b.txt');" "t"
[ ! -f "$WORK/data/$LOC/a/b.txt" ] && ok "and once it commits the file is gone" \
	|| notok "the file is gone" "$WORK/data/$LOC/a/b.txt is still there"
is "removing one that is not there says so" \
   "SELECT gp_sql.remove_file('docs'::regclass, 'never');" "f"

###############################################################################
echo "5. what a path may be"
###############################################################################
refused "a path that climbs out of the directory is refused" \
        "SELECT gp_sql.directory_table_put('docs'::regclass, '../escape', 'x'::bytea);" \
        "must not leave the table's directory"
refused "so is an absolute one" \
        "SELECT gp_sql.directory_table_put('docs'::regclass, '/etc/passwd', 'x'::bytea);" \
        "must be relative"
refused "and an empty one" \
        "SELECT gp_sql.directory_table_put('docs'::regclass, '', 'x'::bytea);" \
        "must not be empty"
is "a path with a dot in a name is fine" \
   "SELECT gp_sql.directory_table_put('docs'::regclass, 'a..b/c.txt', 'ok'::bytea);" "2"
q "SELECT gp_sql.directory_table_put('docs'::regclass, 'once.txt', 'one'::bytea);" > /dev/null
refused "writing over a file is refused, because rolling back could not undo it" \
        "SELECT gp_sql.directory_table_put('docs'::regclass, 'once.txt', 'two'::bytea);" \
        "already exists in directory table"
is "and the bytes that were there are still there" \
   "SELECT convert_from(gp_sql.directory_table_get('docs'::regclass, 'once.txt'), 'UTF8');" "one"

###############################################################################
echo "6. a table that is not a directory table"
###############################################################################
refused "cannot be read as one" \
        "SELECT * FROM gp_sql.directory_table('plain'::regclass);" \
        "is not a directory table"
refused "nor written to as one" \
        "SELECT gp_sql.directory_table_put('plain'::regclass, 'x', 'y'::bytea);" \
        "is not a directory table"
is "and takes ordinary DML, as any table does" \
   "INSERT INTO plain VALUES (1); SELECT count(*) FROM plain;" "1"

###############################################################################
echo "7. only the owner writes files"
###############################################################################
q "CREATE ROLE other LOGIN;" > /dev/null
out=$("$PSQL" -X -q -t -A -d postgres -U other \
	  -c "SELECT gp_sql.directory_table_put('docs'::regclass, 'theirs', 'x'::bytea);" 2>&1)
case "$out" in
	*"must be owner of table docs"*) ok "someone else cannot write one" ;;
	*) notok "someone else cannot write one" "$out" ;;
esac

###############################################################################
echo "8. dropping the table takes its files"
###############################################################################
q "BEGIN; DROP TABLE docs; ROLLBACK;" > /dev/null
[ -d "$WORK/data/$LOC" ] && ok "a drop that rolled back kept the directory" \
	|| notok "a drop that rolled back kept the directory" "$WORK/data/$LOC is gone"
q "DROP TABLE docs;" > /dev/null
[ ! -d "$WORK/data/$LOC" ] && ok "and once it commits the directory is gone" \
	|| notok "the directory is gone" "$(ls -R "$WORK/data/$LOC" 2>&1 | head -3)"
is "nothing is listed any more" \
   "SELECT count(*) FROM gp_sql.directory_tables;" "0"

###############################################################################
echo "9. a standby has every file the node has: they are logged, and it replays them (M4)"
###############################################################################
# A hot standby of this node, streaming from it, with gp_sql preloaded as the
# node has it, which replaying the records takes.
SB="$WORK/standby"
SBPORT=$((PORT + 1))
qs() { "$PSQL" -X -q -t -A -p "$SBPORT" -d postgres -c "$1" 2>&1; }
caught_up() {				# until the standby has replayed what the node wrote
	local lsn
	lsn=$(q "SELECT pg_current_wal_insert_lsn();")
	for _ in $(seq 150); do
		[ "$(qs "SELECT pg_last_wal_replay_lsn() >= '$lsn';")" = t ] && return 0
		sleep 0.2
	done
	return 1
}
BIG="decode(repeat('ab', 1500000), 'hex')"

"$BINDIR/pg_basebackup" -D "$SB" -X stream -c fast -R > "$WORK/basebackup.log" 2>&1 &&
	echo "port = $SBPORT" >> "$SB/postgresql.conf" &&
	"$BINDIR/pg_ctl" -D "$SB" -l "$WORK/standby.log" -w -t 60 start > /dev/null 2>&1
[ "$(qs "SELECT pg_is_in_recovery();")" = t ] && ok "a hot standby of the node, streaming from it" \
	|| notok "a hot standby of the node" "$(tail -3 "$WORK/basebackup.log" "$WORK/standby.log")"

q "SELECT gp_sql.create_directory_table('replicated');
   SELECT gp_sql.directory_table_put('replicated'::regclass, 'dir/one.txt', 'one'::bytea);
   SELECT gp_sql.directory_table_put('replicated'::regclass, 'big.bin', $BIG);
   SELECT gp_sql.directory_table_put('replicated'::regclass, 'empty', ''::bytea);
   SELECT gp_sql.directory_table_put('replicated'::regclass, 'gone.txt', 'gone'::bytea);
   SELECT gp_sql.remove_file('replicated'::regclass, 'gone.txt');" > /dev/null
q "BEGIN; SELECT gp_sql.directory_table_put('replicated'::regclass, 'rolled.txt', 'x'::bytea); ROLLBACK;" > /dev/null
q "SELECT gp_sql.create_directory_table('dropped');
   SELECT gp_sql.directory_table_put('dropped'::regclass, 'f', 'f'::bytea);" > /dev/null
RLOC=$(q "SELECT gp_sql.directory_table_location('replicated'::regclass);")
DLOC=$(q "SELECT gp_sql.directory_table_location('dropped'::regclass);")
q "DROP TABLE dropped;" > /dev/null
caught_up || notok "the standby catches up"

for f in dir/one.txt big.bin empty; do
	cmp -s "$WORK/data/$RLOC/$f" "$SB/$RLOC/$f" || { notok "the standby has $f, byte for byte" "$(ls -l "$SB/$RLOC/$f" 2>&1)"; f=; break; }
done
[ -n "$f" ] && ok "the standby has each file written, byte for byte, 1.5 MB of one in two records and nothing of another"
[ ! -e "$SB/$RLOC/gone.txt" ] && [ ! -e "$SB/$RLOC/rolled.txt" ] \
	&& ok "and not the one removed, nor the one whose put rolled back" \
	|| notok "the standby has what was removed or rolled back" "$(ls "$SB/$RLOC")"
[ -n "$DLOC" ] && [ ! -e "$SB/$DLOC" ] && ok "nor the directory of a table dropped" \
	|| notok "the standby has a dropped table's directory" "$DLOC"
is "the standby reads a file as the node does" \
   "SELECT convert_from(gp_sql.directory_table_get('replicated'::regclass, 'dir/one.txt'), 'UTF8');" "one"
out=$(qs "SELECT md5(gp_sql.directory_table_get('replicated'::regclass, 'big.bin')) = md5($BIG);")
[ "$out" = t ] && ok "and on the standby, too" || notok "reading a file on the standby" "$out"
out=$(qs "SELECT gp_sql.directory_table_put('replicated'::regclass, 'no.txt', 'no'::bytea);")
case "$out" in
	*"cannot write a file of a directory table during recovery"*) ok "the standby refuses to write one of its own" ;;
	*) notok "the standby refuses to write a file" "$out" ;;
esac
"$BINDIR/pg_ctl" -D "$SB" -m fast -w stop > /dev/null 2>&1

###############################################################################
echo "10. a server replaying the records without gp_sql stops, and with it recovers (check 12)"
###############################################################################
# A record the next recovery has to replay: written after the checkpoint it
# starts from, the node then stopped without a checkpoint of its own.
q "CHECKPOINT;
   SELECT gp_sql.directory_table_put('replicated'::regclass, 'late.txt', 'late'::bytea);" > /dev/null
"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate -w stop > /dev/null 2>&1
rm -f "$WORK/data/$RLOC/late.txt"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log-nogpsql" -o "-c shared_preload_libraries=gp_core" \
	-w -t 60 start > /dev/null 2>&1
started=$?
out=$(grep -o 'FATAL:  resource manager with ID 198 not registered' "$WORK/log-nogpsql" | head -1)
[ "$started" -ne 0 ] && [ -n "$out" ] \
	&& ok "without gp_sql preloaded, recovery stops at its record, with PostgreSQL's FATAL" \
	|| notok "recovery without gp_sql" "started=$started $(grep -E 'FATAL|PANIC' "$WORK/log-nogpsql" | head -3)"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1
is "with it, recovery replays the record, and the file is back" \
   "SELECT convert_from(gp_sql.directory_table_get('replicated'::regclass, 'late.txt'), 'UTF8');" "late"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
