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
# whether a file and its row stay in step when a transaction rolls back, to
# a savepoint, after it is prepared, across a restart and a crash; and, on a
# cluster, whether each file is on the segment its path hashes to, through
# two-phase commit, a restart between the phases, a drop, a failover and a
# storage server.
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
	for d in "$WORK"/cluster/node*; do
		[ -d "$d" ] && "$BINDIR/pg_ctl" -D "$d" -m immediate stop > /dev/null 2>&1
	done
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
	echo "max_prepared_transactions = 10"
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
refused "and so is UPDATE of a column that describes the file, in Cloudberry's words" \
        "UPDATE docs SET size = 0;" "Only allow to update directory \"tag\" column."
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
# put is not STRICT, its tag being NULL where none is given: a NULL it reads
# is refused before it is read, where it once crashed the server (a lost
# connection, here)
refused "a NULL path is refused" \
        "SELECT gp_sql.directory_table_put('docs'::regclass, NULL, 'x'::bytea);" \
        "file path must not be null"
refused "so is NULL content, an empty file's being ''" \
        "SELECT gp_sql.directory_table_put('docs'::regclass, 'null.txt', NULL);" \
        "file content must not be null"
refused "and a NULL table, which reached the others with no table at all" \
        "SELECT gp_sql.directory_table_put(NULL, NULL, NULL);" \
        "directory table must not be null"
isl "a NULL tag is no tag: the file is written" \
    "SELECT gp_sql.directory_table_put('docs'::regclass, 'untagged.txt', ''::bytea, NULL);
     SELECT size || ',' || coalesce(tag, 'none') FROM docs WHERE relative_path = 'untagged.txt';" "0,none"
is "a path with a dot in a name is fine" \
   "SELECT gp_sql.directory_table_put('docs'::regclass, 'a..b/c.txt', 'ok'::bytea);" "2"
q "SELECT gp_sql.directory_table_put('docs'::regclass, 'once.txt', 'one'::bytea);" > /dev/null
refused "a second put of a path is its row's duplicate key, as in Cloudberry" \
        "SELECT gp_sql.directory_table_put('docs'::regclass, 'once.txt', 'two'::bytea);" \
        "duplicate key value violates unique constraint \"docs_pkey\""
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
out=$("$PSQL" -X -q -t -A -d postgres -U other \
	  -c "SELECT gp_sql.directory_table_put(NULL, NULL, NULL);" 2>&1)
case "$out" in
	*"directory table must not be null"*) ok "and their NULLs are refused alike" ;;
	*) notok "their NULLs are refused alike" "$out" ;;
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

###############################################################################
echo "11. the offline tools leave a directory table's files alone (O23)"
###############################################################################
# PostgreSQL 19's pg_checksums walks every file under a database directory as
# a relation's pages: it stops at a file whose name is no segment number, and
# with --enable writes a checksum into every 8K of one that is whole blocks.
# gp_sql marks <relid>_dirtable as its own while the postmaster loads it.
if grep -qx '_dirtable' "$WORK/data/extension_marks" 2>/dev/null; then
	ok "the postmaster that loaded gp_sql marked its directories"
else
	notok "extension_marks should hold _dirtable" "$(cat "$WORK/data/extension_marks" 2>&1)"
fi
q "SELECT gp_sql.create_directory_table('offline');" > /dev/null
OLOC=$(q "SELECT gp_sql.directory_table_location('offline'::regclass);")
q "SELECT gp_sql.directory_table_put('offline'::regclass, 'block.bin', decode(repeat('ab', 8192), 'hex'));" > /dev/null
q "SELECT gp_sql.directory_table_put('offline'::regclass, 'notes.v2.txt', 'a name with dots'::bytea);" > /dev/null
q "CHECKPOINT;" > /dev/null
blocksum=$(md5sum < "$WORK/data/$OLOC/block.bin")
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w stop > /dev/null 2>&1
"$BINDIR/pg_checksums" --check -D "$WORK/data" > "$WORK/checksums.log" 2>&1 \
	&& ok "pg_checksums --check passes over a directory table's files" \
	|| notok "pg_checksums --check with a directory table" "$(tail -3 "$WORK/checksums.log")"
"$BINDIR/pg_checksums" --disable -D "$WORK/data" > /dev/null 2>&1
"$BINDIR/pg_checksums" --enable -D "$WORK/data" > "$WORK/checksums-enable.log" 2>&1 \
	&& [ "$(md5sum < "$WORK/data/$OLOC/block.bin")" = "$blocksum" ] \
	&& ok "and --enable writes nothing into a file of whole blocks" \
	|| notok "pg_checksums --enable with a directory table" "$(tail -3 "$WORK/checksums-enable.log")"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1
is "the file reads back as it was written" \
   "SELECT length(gp_sql.directory_table_get('offline'::regclass, 'block.bin'));" "8192"

###############################################################################
echo "12. a file follows its row: savepoints, a removal and a put of one path, two phases, a crash and the sweep"
###############################################################################
# One session of several statements, each sent on its own, as psql sends
# its -c options: what PREPARE TRANSACTION and its second phase need.
qs() {
	local args=()
	for c in "$@"; do args+=(-c "$c"); done
	"$PSQL" -X -q -t -A -d postgres "${args[@]}" 2>&1
}
put() { echo "SELECT gp_sql.directory_table_put('follow'::regclass, '$1', '$2'::bytea)"; }
content() { q "SELECT coalesce(convert_from(gp_sql.directory_table_get('follow'::regclass, '$1'), 'UTF8'), 'NULL');"; }
there() { [ -f "$WORK/data/$FLOC/$1" ]; }
moved() { ls "$WORK/data/$FLOC" | grep -c 'gp_removed$'; }

q "SELECT gp_sql.create_directory_table('follow');" > /dev/null
FLOC=$(q "SELECT gp_sql.directory_table_location('follow'::regclass);")

isl "a put rolled back to its savepoint takes its file with it at once" \
    "BEGIN; SAVEPOINT a; $(put sp.txt x); ROLLBACK TO a;
     SELECT pg_stat_file('$FLOC/sp.txt', true) IS NULL; COMMIT;" "t"
q "BEGIN; SAVEPOINT a; $(put rel.txt x); RELEASE a; ROLLBACK;" > /dev/null
! there rel.txt && ok "a put released to its parent goes when the parent rolls back" \
	|| notok "a put released to a parent that rolled back" "$(ls "$WORK/data/$FLOC")"
q "$(put keep.txt kept);" > /dev/null
q "BEGIN; SAVEPOINT a; SELECT gp_sql.remove_file('follow'::regclass, 'keep.txt'); ROLLBACK TO a; COMMIT;" > /dev/null
[ "$(content keep.txt)" = kept ] && ok "a removal rolled back to its savepoint keeps its row and its file" \
	|| notok "a removal rolled back to its savepoint" "$(content keep.txt)"

# Cloudberry's own test removes a file and puts its path again in one
# transaction: the put writes a new file, and the old one waits, moved
# aside, for the transaction's end.
q "$(put swap.txt old);" > /dev/null
q "BEGIN; SELECT gp_sql.remove_file('follow'::regclass, 'swap.txt'); SAVEPOINT s;
   $(put swap.txt new); RELEASE s; COMMIT;" > /dev/null
[ "$(content swap.txt)" = new ] && [ "$(moved)" = 0 ] \
	&& ok "a removal and a put of one path, committed: the new file, and nothing moved aside is left" \
	|| notok "a removal and a put of one path, committed" "$(content swap.txt) / $(ls "$WORK/data/$FLOC")"
q "BEGIN; SELECT gp_sql.remove_file('follow'::regclass, 'swap.txt'); $(put swap.txt newer); ROLLBACK;" > /dev/null
[ "$(content swap.txt)" = new ] && [ "$(moved)" = 0 ] \
	&& ok "rolled back: the old file is back under its row" \
	|| notok "a removal and a put of one path, rolled back" "$(content swap.txt) / $(ls "$WORK/data/$FLOC")"
is "and its row describes it" \
   "SELECT md5 = md5(gp_sql.directory_table_get('follow'::regclass, 'swap.txt')) FROM follow WHERE relative_path = 'swap.txt';" "t"
q "BEGIN; SELECT gp_sql.remove_file('follow'::regclass, 'swap.txt'); SAVEPOINT s; $(put swap.txt x);
   ROLLBACK TO s; COMMIT;" > /dev/null
! there swap.txt && [ "$(moved)" = 0 ] && [ "$(q "SELECT count(*) FROM follow WHERE relative_path = 'swap.txt';")" = 0 ] \
	&& ok "the put rolled back to its savepoint and the removal committed: neither row nor file" \
	|| notok "a put rolled back to its savepoint after a removal that committed" "$(ls "$WORK/data/$FLOC")"

out=$(qs "BEGIN" "$(put p1.txt one)" "PREPARE TRANSACTION 'dt_p1'" "ROLLBACK PREPARED 'dt_p1'" \
		 "SELECT pg_stat_file('$FLOC/p1.txt', true) IS NULL" | tail -1)
[ "$out" = t ] && ok "a put prepared and rolled back by the backend that prepared it: its file goes then" \
	|| notok "ROLLBACK PREPARED of a put in the same backend" "$out"
out=$(qs "BEGIN" "$(put p2.txt two)" "PREPARE TRANSACTION 'dt_p2'" "COMMIT PREPARED 'dt_p2'" | tail -1)
[ "$(content p2.txt)" = two ] && ok "committed, it stays" || notok "COMMIT PREPARED of a put" "$out"
out=$(qs "BEGIN" "SELECT gp_sql.remove_file('follow'::regclass, 'p2.txt')" "PREPARE TRANSACTION 'dt_p3'" \
		 "ROLLBACK PREPARED 'dt_p3'" | tail -1)
[ "$(content p2.txt)" = two ] && [ "$(moved)" = 0 ] \
	&& ok "a removal prepared and rolled back: its file is back at once" \
	|| notok "ROLLBACK PREPARED of a removal" "$out / $(ls "$WORK/data/$FLOC")"
out=$(qs "BEGIN" "SELECT gp_sql.remove_file('follow'::regclass, 'p2.txt')" "PREPARE TRANSACTION 'dt_p4'" \
		 "COMMIT PREPARED 'dt_p4'" | tail -1)
! there p2.txt && [ "$(moved)" = 0 ] && ok "and committed, gone at once" \
	|| notok "COMMIT PREPARED of a removal" "$out / $(ls "$WORK/data/$FLOC")"

# The second phase another backend runs, and one after a restart, leave
# what it would remove to the sweep.
qs "BEGIN" "$(put p5.txt five)" "PREPARE TRANSACTION 'dt_p5'" > /dev/null
q "ROLLBACK PREPARED 'dt_p5';" > /dev/null
there p5.txt && [ "$(content p5.txt)" = NULL ] \
	&& ok "rolled back by another backend, the file waits for the sweep, and reads as none meanwhile" \
	|| notok "ROLLBACK PREPARED of a put in another backend" "$(ls "$WORK/data/$FLOC") / $(content p5.txt)"
is "a put of its path writes over it" "$(put p5.txt again);" "5"
[ "$(content p5.txt)" = again ] && ok "and reads as what was put" || notok "a put over garbage" "$(content p5.txt)"
# A removal takes the table's AccessExclusiveLock, as Cloudberry's does,
# which a prepared one keeps to its second phase: two, in two tables.
q "$(put p6.txt six);
   SELECT gp_sql.create_directory_table('follow2');
   SELECT gp_sql.directory_table_put('follow2'::regclass, 'p7.txt', 'seven'::bytea);" > /dev/null
F2LOC=$(q "SELECT gp_sql.directory_table_location('follow2'::regclass);")
qs "BEGIN" "SELECT gp_sql.remove_file('follow'::regclass, 'p6.txt')" "PREPARE TRANSACTION 'dt_p6'" > /dev/null
qs "BEGIN" "SELECT gp_sql.remove_file('follow2'::regclass, 'p7.txt')" "PREPARE TRANSACTION 'dt_p7'" > /dev/null
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w restart > /dev/null 2>&1
q "ROLLBACK PREPARED 'dt_p6';" > /dev/null
q "COMMIT PREPARED 'dt_p7';" > /dev/null
[ "$(moved)" = 1 ] && [ "$(ls "$WORK/data/$F2LOC" | grep -c 'gp_removed$')" = 1 ] &&
	[ "$(content p6.txt)" = six ] \
	&& ok "after a restart between the phases both files are aside, and the one whose row is back reads from there" \
	|| notok "a restart between the phases" "$(ls "$WORK/data/$FLOC" "$WORK/data/$F2LOC") / $(content p6.txt)"
is "the sweep moves the one whose row came back and removes the other" \
   "SELECT gp_sql.directory_table_sweep();" "1"
[ "$(content p6.txt)" = six ] && [ "$(moved)" = 0 ] && [ -z "$(ls "$WORK/data/$F2LOC")" ] \
	&& ok "and each file is where its row says" \
	|| notok "the sweep after a restart between the phases" "$(ls "$WORK/data/$FLOC" "$WORK/data/$F2LOC") / $(content p6.txt)"

# A crash between a put's write and its commit: the file stays, no row
# describes it.
"$PSQL" -X -q -d postgres -c "BEGIN" -c "$(put crash.txt lost)" -c "SELECT pg_sleep(60)" > /dev/null 2>&1 &
for _ in $(seq 100); do there crash.txt && break; sleep 0.1; done
"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate -w stop > /dev/null 2>&1
wait
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1
there crash.txt && [ "$(content crash.txt)" = NULL ] \
	&& ok "a crash after a put's write leaves its file, which reads as none" \
	|| notok "a crash after a write" "$(ls "$WORK/data/$FLOC") / $(content crash.txt)"
mkdir -p "$WORK/data/$(dirname "$FLOC")/4000000000_dirtable/sub"
echo stray > "$WORK/data/$(dirname "$FLOC")/4000000000_dirtable/sub/f"
is "the sweep removes it, and the directory of a table that is gone" \
   "SELECT gp_sql.directory_table_sweep();" "2"
! there crash.txt && [ ! -e "$WORK/data/$(dirname "$FLOC")/4000000000_dirtable" ] \
	&& ok "and both are gone" || notok "the sweep of a crash's file and a dropped table's directory" "$(ls "$WORK/data/$FLOC")"
refused "the sweep is a superuser's" \
        "SET ROLE other; SELECT gp_sql.directory_table_sweep();" "permission denied for function directory_table_sweep"

###############################################################################
echo "13. Cloudberry's COPY of a file, in and out"
###############################################################################
printf 'from a file' > "$WORK/src.bin"
is "COPY BINARY ... FROM a file, with its path and tag" \
   "COPY BINARY follow FROM '$WORK/src.bin' 'copied/a.bin' WITH TAG 'copy';
    SELECT tag || ' ' || size FROM follow WHERE relative_path = 'copied/a.bin';" "copy 11"
out=$(printf 'from the client' | "$PSQL" -X -q -d postgres -c "COPY BINARY follow FROM STDIN 'copied/b.bin'" 2>&1)
[ -z "$out" ] && [ "$(content copied/b.bin)" = "from the client" ] \
	&& ok "COPY BINARY ... FROM STDIN, the client's stream" || notok "COPY FROM STDIN" "$out / $(content copied/b.bin)"
out=$(printf 'as psql puts it' > "$WORK/c.bin"; "$PSQL" -X -q -d postgres -c "\\copy binary follow from '$WORK/c.bin' 'copied/c.bin' with tag 'psql'" 2>&1)
[ -z "$out" ] && [ "$(content copied/c.bin)" = "as psql puts it" ] \
	&& ok "and psql's \\copy binary ... from ... with tag, which gpdirtableload writes" || notok "psql's \\copy" "$out"
is "COPY BINARY ... FROM PROGRAM" \
   "COPY BINARY follow FROM PROGRAM 'printf program' 'copied/d.bin';
    SELECT convert_from(gp_sql.directory_table_get('follow'::regclass, 'copied/d.bin'), 'UTF8');" "program"
q "COPY BINARY DIRECTORY TABLE follow 'copied/a.bin' TO '$WORK/out.bin';" > /dev/null
cmp -s "$WORK/src.bin" "$WORK/out.bin" && ok "COPY BINARY DIRECTORY TABLE ... TO a file, byte for byte" \
	|| notok "COPY BINARY DIRECTORY TABLE TO a file" "$(ls -l "$WORK/out.bin" 2>&1)"
out=$("$PSQL" -X -q -d postgres -c "COPY BINARY DIRECTORY TABLE follow 'copied/b.bin' TO STDOUT" 2>&1)
[ "$out" = "from the client" ] && ok "and TO STDOUT, the bytes as they are" || notok "COPY TO STDOUT" "$out"
refused "a COPY of a directory table from a file names its path, as Cloudberry asks" \
        "COPY follow FROM '$WORK/src.bin';" "Copy from directory table file name can't be null."
refused "and is BINARY" "COPY follow FROM '$WORK/src.bin' 'x';" "Only support copy binary from directory table."
refused "and takes no other option" "COPY BINARY follow FROM '$WORK/src.bin' 'x' (delimiter ',');" \
        "option \"delimiter\" not recognized"
refused "nor a path of an ordinary table" "COPY BINARY plain FROM '$WORK/src.bin' 'x';" \
        "\"plain\" is not a directory table"
out=$("$PSQL" -X -q -t -A -d postgres -c "COPY follow (relative_path, size) TO STDOUT" 2>&1 | wc -l)
[ "$out" -ge 4 ] && ok "its rows still go to the client and come back as pg_dump moves them" \
	|| notok "COPY of a directory table's rows" "$out"
out=$("$PSQL" -X -q -d postgres -U other -c "COPY BINARY follow FROM STDIN 'theirs'" < /dev/null 2>&1)
case "$out" in
	*"must be owner of table follow"*) ok "someone else cannot put one by COPY either" ;;
	*) notok "COPY by someone else" "$out" ;;
esac

###############################################################################
echo "14. on a cluster: each file and its row on the segment its path hashes to"
###############################################################################
# A coordinator and two primaries, each with a mirror streaming from it, as
# the fts suite makes them, every node preloading the storage suite's
# handler, gp_storage_probe; the one node above stopped first.
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w stop > /dev/null 2>&1
CL="$WORK/cluster"
NSEG=2
SLOT=internal_wal_replication_slot
cdir()  { echo "$CL/node$1"; }
cport() { echo $((PORT + 10 + $1)); }
cq()    { "$PSQL" -X -q -t -A -p "$(cport "$1")" -d postgres -c "$2" 2>&1; }
ccontent() { if [ "$1" -eq 0 ]; then echo -1; elif [ "$1" -le "$NSEG" ]; then echo $(($1 - 1)); else echo $(($1 - NSEG - 1)); fi; }
cstart() { "$BINDIR/pg_ctl" -D "$(cdir "$1")" -l "$CL/node$1.log" -w -t 60 start > /dev/null 2>&1; }
cwait() {					# cwait <node> <sql> <want> [seconds]
	local out
	for _ in $(seq $((${4:-30} * 5))); do
		out=$(cq "$1" "$2")
		[ "$out" = "$3" ] && return 0
		sleep 0.2
	done
	echo "$out"
	return 1
}
make_mirror() {				# make_mirror <n> <of n>
	local m="$1" p="$2"
	"$BINDIR/pg_basebackup" -D "$(cdir "$m")" -p "$(cport "$p")" -X stream -c fast \
		-C -S "$SLOT" > "$CL/basebackup$m.log" 2>&1 || return 1
	grep -v -E '^(port|gp\.dbid|primary_conninfo|primary_slot_name|hot_standby) ' \
		"$(cdir "$m")/postgresql.auto.conf" > "$CL/auto.conf"
	{
		cat "$CL/auto.conf"
		echo "port = $(cport "$m")"
		echo "gp.dbid = $((m + 1))"
		echo "hot_standby = on"
		echo "primary_conninfo = 'host=$SOCK port=$(cport "$p") application_name=gp_walreceiver'"
		echo "primary_slot_name = '$SLOT'"
	} > "$(cdir "$m")/postgresql.auto.conf"
	touch "$(cdir "$m")/standby.signal"
	cstart "$m"
}
# The nodes a file of a directory table at location $2 is on.
on_nodes() {				# on_nodes <relative path> <location> [nodes]
	for n in ${3:-0 1 2}; do [ -f "$(cdir "$n")/$2/$1" ] && printf '%s' "$n"; done
}
caught_up_mirrors() {		# until each mirror has replayed what its primary wrote
	local lsn
	for n in $(seq 1 "$NSEG"); do
		lsn=$(cq "$n" "SELECT pg_current_wal_insert_lsn()")
		cwait $((n + NSEG)) "SELECT pg_last_wal_replay_lsn() >= '$lsn'" t > /dev/null || return 1
	done
}

mkdir -p "$CL" "$WORK/probe_root" "$WORK/probe_space"
{
	echo "# dbid content role host port datadir"
	for n in $(seq 0 $((2 * NSEG))); do
		role=p; [ "$n" -gt "$NSEG" ] && role=m
		echo "$((n + 1)) $(ccontent "$n") $role $SOCK $(cport "$n") $(cdir "$n")"
	done
} > "$CL/gp_cluster.conf"
for n in $(seq 0 "$NSEG"); do
	"$BINDIR/initdb" -D "$(cdir "$n")" -N --locale=C --encoding=UTF8 > "$CL/initdb$n.log" 2>&1
	{
		echo "shared_preload_libraries = 'gp_core,gp_sql,gp_storage_probe'"
		echo "unix_socket_directories = '$SOCK'"
		echo "listen_addresses = ''"
		echo "port = $(cport "$n")"
		echo "fsync = off"
		echo "max_prepared_transactions = 10"
		echo "gp.cluster_config = '$CL/gp_cluster.conf'"
		echo "gp.dbid = $((n + 1))"
		echo "gp.cluster_secret = 'dirtable-cluster-$RANDOM$RANDOM$RANDOM'"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$(cdir "$n")/postgresql.auto.conf"
done
secret=$(grep gp.cluster_secret "$(cdir 0)/postgresql.auto.conf")
for n in $(seq 1 "$NSEG"); do
	sed -i "s/^gp.cluster_secret = .*/$secret/" "$(cdir "$n")/postgresql.auto.conf"
done
started=1
for n in $(seq 1 "$NSEG"); do cstart "$n" || started=0; done
for n in $(seq 1 "$NSEG"); do make_mirror $((n + NSEG)) "$n" || started=0; done
cstart 0 || started=0
out=$(cq 0 "CREATE EXTENSION gp_core; CREATE EXTENSION gp_sql; CREATE EXTENSION gp_inject_fault;")
for n in $(seq 1 "$NSEG"); do
	cwait "$n" "SELECT count(*) FROM pg_stat_replication WHERE application_name = 'gp_walreceiver' AND state = 'streaming'" 1 \
		> /dev/null || started=0
done
cq 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
[ "$started" -eq 1 ] && [ -z "$out" ] && ok "a coordinator and two primaries start, each primary with a mirror streaming from it" \
	|| notok "the cluster starts" "$out $(tail -q -n 3 "$CL"/node*.log)"

out=$(cq 0 "CREATE DIRECTORY TABLE cdocs; SELECT gp_sql.distribution('cdocs'::regclass);")
CLOC=$(cq 0 "SELECT gp_sql.directory_table_location('cdocs'::regclass);")
[ "$out" = "(relative_path)" ] && [ ! -e "$(cdir 0)/$CLOC" ] \
	&& ok "a directory table is distributed by its relative path, and the coordinator makes it no directory" \
	|| notok "a directory table on a cluster" "$out / $(ls -d "$(cdir 0)/$CLOC" 2>&1)"
for i in 1 2 3 4 5 6; do cq 0 "SELECT gp_sql.directory_table_put('cdocs', 'f$i.txt', 'file $i')" > /dev/null; done
bad=""
while IFS='|' read -r seg path; do
	[ "$(on_nodes "$path" "$CLOC")" = "$((seg + 1))" ] || bad="$bad $path:$seg:$(on_nodes "$path" "$CLOC")"
done < <(cq 0 "SELECT gp_segment_id, relative_path FROM cdocs")
[ -z "$bad" ] && [ "$(cq 0 "SELECT count(DISTINCT gp_segment_id) FROM cdocs")" = 2 ] \
	&& ok "each file is on its row's segment and no other node, and each segment has some" \
	|| notok "where the files are" "$bad"
out=$(cq 0 "SELECT string_agg(relative_path || '=' || convert_from(content, 'UTF8'), ',' ORDER BY relative_path) FROM directory_table('cdocs')")
[ "$out" = "f1.txt=file 1,f2.txt=file 2,f3.txt=file 3,f4.txt=file 4,f5.txt=file 5,f6.txt=file 6" ] \
	&& ok "directory_table() runs on every segment, each reading its own" || notok "directory_table() on a cluster" "$out"
out=$(cq 0 "SELECT convert_from(gp_sql.directory_table_get('cdocs', 'f4.txt'), 'UTF8')")
[ "$out" = "file 4" ] && ok "a get reads the file on its segment" || notok "a get on a cluster" "$out"
out=$(cq 0 "SELECT gp_sql.directory_table_put('cdocs', 'f4.txt', 'again')")
case "$out" in
	*'duplicate key value violates unique constraint "cdocs_pkey"'*) ok "a second put of a path is refused on its segment" ;;
	*) notok "a second put of a path on a cluster" "$out" ;;
esac
out=$(cq 0 "BEGIN; SELECT gp_sql.directory_table_put('cdocs', 'rolled.txt', 'x'); ROLLBACK;
            SELECT count(*) FROM cdocs WHERE relative_path = 'rolled.txt';" | tail -1)
[ "$out" = "0" ] && [ -z "$(on_nodes rolled.txt "$CLOC")" ] \
	&& ok "a put rolled back leaves neither row nor file anywhere" || notok "a rollback on a cluster" "$out"

# Paths known to go to each segment, for a table of the same distribution.
S0=$(cq 0 "SELECT relative_path FROM cdocs WHERE gp_segment_id = 0 ORDER BY 1 LIMIT 1")
S1=$(cq 0 "SELECT relative_path FROM cdocs WHERE gp_segment_id = 1 ORDER BY 1 LIMIT 1")
two() {						# two <table> <content>: a put of S0 and S1, one statement each
	echo "SELECT gp_sql.directory_table_put('$1', '$S0', '$2'); SELECT gp_sql.directory_table_put('$1', '$S1', '$2');"
}
cq 0 "CREATE DIRECTORY TABLE ctwo; CREATE DIRECTORY TABLE crb; CREATE DIRECTORY TABLE cab;
      CREATE DIRECTORY TABLE crs;" > /dev/null
loc() { cq 0 "SELECT gp_sql.directory_table_location('$1'::regclass)"; }
cq 0 "BEGIN; $(two ctwo both) COMMIT;" > /dev/null
[ "$(on_nodes "$S0" "$(loc ctwo)")" = 1 ] && [ "$(on_nodes "$S1" "$(loc ctwo)")" = 2 ] \
	&& ok "two files on two segments, committed in two phases" || notok "a two-phase put" "$(ls -R "$CL"/node[12]/"$(loc ctwo)" 2>&1)"
cq 0 "BEGIN; $(two crb gone) ROLLBACK;" > /dev/null
[ -z "$(on_nodes "$S0" "$(loc crb)")$(on_nodes "$S1" "$(loc crb)")" ] \
	&& ok "rolled back: neither" || notok "a rollback of two segments' puts"
out=$(cq 0 "SET gp.debug_abort_after_distributed_prepared = on; BEGIN; $(two cab gone) COMMIT;")
case "$out" in
	*"Raise an error as directed by Debug_abort_after_distributed_prepared"*)
		[ -z "$(on_nodes "$S0" "$(loc cab)")$(on_nodes "$S1" "$(loc cab)")" ] \
			&& ok "aborted after both prepared: each segment's second phase takes its file at once" \
			|| notok "aborted after the prepare" "$(ls -R "$CL"/node[12]/"$(loc cab)" 2>&1)" ;;
	*) notok "aborted after the prepare" "$out" ;;
esac

# The coordinator's crash after its commit and before the second phase: its
# recovery process commits each segment's part, in a backend that did not
# prepare it.
cq 0 "SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'panic', 1)" > /dev/null
cq 0 "BEGIN; $(two crs kept) COMMIT;" > /dev/null
cwait 1 "SELECT count(*) FROM pg_prepared_xacts" 0 60 > /dev/null
cwait 2 "SELECT count(*) FROM pg_prepared_xacts" 0 60 > /dev/null
out=$(cwait 0 "SELECT string_agg(convert_from(content, 'UTF8'), ',') FROM directory_table('crs')" "kept,kept" 60)
[ -z "$out" ] && [ "$(on_nodes "$S0" "$(loc crs)")$(on_nodes "$S1" "$(loc crs)")" = 12 ] \
	&& ok "a restart between the phases: the recovery commits both, and each file is on its segment" \
	|| notok "a restart between the phases" "$out"
cq 0 "SELECT gp_inject_fault('dtm_broadcast_commit_prepared', 'panic', 1)" > /dev/null
cq 0 "BEGIN; SELECT gp_sql.remove_file('crs', '$S0'); SELECT gp_sql.remove_file('crs', '$S1'); COMMIT;" > /dev/null
cwait 1 "SELECT count(*) FROM pg_prepared_xacts" 0 60 > /dev/null
cwait 2 "SELECT count(*) FROM pg_prepared_xacts" 0 60 > /dev/null
out=$(cwait 0 "SELECT count(*) FROM crs" 0 60)
aside=$(ls "$(cdir 1)/$(loc crs)" "$(cdir 2)/$(loc crs)" 2>/dev/null | grep -c 'gp_removed$')
[ -z "$out" ] && [ "$aside" = 2 ] && ok "and of two removals: rows gone, their files aside for the sweep" \
	|| notok "removals across a restart between the phases" "$out / $aside"
out=$(cq 0 "SELECT gp_sql.directory_table_sweep()")
echo stray > "$(cdir 2)/$CLOC/stray"
out2=$(cq 0 "SELECT gp_sql.directory_table_sweep()")
[ "$out" = 2 ] && [ "$out2" = 1 ] && [ -z "$(find "$(cdir 1)/$(loc crs)" "$(cdir 2)/$(loc crs)" -type f)" ] &&
	[ ! -e "$(cdir 2)/$CLOC/stray" ] \
	&& ok "the sweep, from the coordinator, removes them on every segment, and a stray file" \
	|| notok "the sweep on a cluster" "$out / $out2"

caught_up_mirrors
[ "$(on_nodes "$S0" "$(loc ctwo)" "3 4")" = 3 ] && [ "$(on_nodes "$S1" "$(loc ctwo)" "3 4")" = 4 ] \
	&& ok "each mirror has its primary's files" || notok "the mirrors' files" "$(ls -R "$CL"/node[34]/"$(loc ctwo)" 2>&1)"
TWOLOC=$(loc ctwo)
cq 0 "DROP DIRECTORY TABLE ctwo WITH CONTENT;" > /dev/null
caught_up_mirrors
left=$(for n in 0 1 2 3 4; do [ -e "$(cdir "$n")/$TWOLOC" ] && printf '%s ' "$n"; done)
[ -z "$left" ] && ok "DROP DIRECTORY TABLE clears every segment and every mirror" \
	|| notok "a dropped directory table's directories" "left on nodes $left"

out=$(cq 1 "SELECT gp_sql.directory_table_put('cdocs', 'u.txt', 'u')")
out2=$(cq 1 "SELECT gp_sql.remove_file('cdocs', '$S0')")
case "$out$out2" in
	*"directory_table_put() could only be called on QD"*"remove_file() could only be called on QD"*)
		ok "a segment's own session writes and removes no file, in Cloudberry's words" ;;
	*) notok "writes in a segment's own session" "$out / $out2" ;;
esac
out=$(cq 1 "SELECT convert_from(gp_sql.directory_table_get('cdocs', '$S0'), 'UTF8')")
out2=$(cq 1 "SELECT gp_sql.directory_table_get('cdocs', '$S1')")
case "$out2" in
	*"is on segment 1"*) [ -n "$out" ] && ok "it reads its own files, and not another segment's" \
		|| notok "a segment's own read" "$out" ;;
	*) notok "a segment's read of another's file" "$out2" ;;
esac
out=$(cq 0 "ALTER TABLE cdocs SET DISTRIBUTED RANDOMLY;")
case "$out" in
	*"ALTER action SET DISTRIBUTED BY cannot be performed on relation \"cdocs\""*) ok "its distribution cannot change, its files not moving" ;;
	*) notok "SET DISTRIBUTED of a directory table" "$out" ;;
esac
out=$(cq 0 "INSERT INTO cdocs VALUES ('by hand');")
out2=$(cq 0 "UPDATE cdocs SET relative_path = 'moved' WHERE relative_path = '$S0';")
case "$out$out2" in
	*"cannot change directory table \"cdocs\""*"Only allow to update directory \"tag\" column."*)
		ok "INSERT and an UPDATE of its path are refused on the coordinator, whatever carries them out" ;;
	*) notok "DML on a cluster" "$out / $out2" ;;
esac
printf 'from a file' > "$WORK/csrc.bin"
out=$(cq 0 "COPY BINARY cdocs FROM '$WORK/csrc.bin' 'copied.bin' WITH TAG 'c';
            COPY BINARY DIRECTORY TABLE cdocs 'copied.bin' TO '$WORK/cout.bin';")
cmp -s "$WORK/csrc.bin" "$WORK/cout.bin" && ok "COPY BINARY puts a file on its segment, and takes it out again" \
	|| notok "COPY on a cluster" "$out"

# A storage server's tablespace: the handler's I/O on the coordinator, the
# row on its segment.
out=$(cq 0 "SELECT gp_sql.create_storage_server('probe_srv', '{\"protocol\": \"probe\", \"root\": \"$WORK/probe_root\", \"secret\": \"sesame\"}');
            SELECT gp_sql.create_storage_user_mapping('probe_srv', CURRENT_USER, '{\"secret\": \"sesame\"}');")
cq 0 "CREATE TABLESPACE probe_space LOCATION '$WORK/probe_space' WITH (gp.server = 'probe_srv');" > /dev/null
out=$(cq 0 "CREATE DIRECTORY TABLE cfar TABLESPACE probe_space;
            SELECT gp_sql.directory_table_put('cfar', '$S1', 'far away');
            SELECT gp_segment_id FROM cfar;")
FARLOC=$(cq 0 "SELECT gp_sql.directory_table_location('cfar'::regclass)")
[ "$(echo "$out" | tail -1)" = 1 ] && [ -f "$WORK/probe_root/$FARLOC/$S1" ] &&
	[ -z "$(find "$WORK/probe_space" "$CL" -name '*_dirtable' -path "*$(basename "$FARLOC")*")" ] \
	&& ok "a storage server's file is written through its handler, and its row goes to the path's segment" \
	|| notok "a storage server's file on a cluster" "$out / $(ls -R "$WORK/probe_root" 2>&1 | head -5)"
out=$(cq 0 "SELECT convert_from(gp_sql.directory_table_get('cfar', '$S1'), 'UTF8');
            BEGIN; SELECT gp_sql.directory_table_put('cfar', '$S0', 'gone'); ROLLBACK;
            SELECT gp_sql.remove_file('cfar', '$S1');")
[ "$out" = "far away
4
t" ] && [ -z "$(ls "$WORK/probe_root/$FARLOC")" ] \
	&& ok "read back, and a rollback and a removal take their files off the server" \
	|| notok "a storage server's files on a cluster" "$out / $(ls "$WORK/probe_root/$FARLOC")"

# A failover: content 0's primary stops, FTS promotes its mirror, which has
# the files from its primary's WAL.
"$BINDIR/pg_ctl" -D "$(cdir 1)" -m immediate -w stop > /dev/null 2>&1
cq 0 "SELECT gp_request_fts_probe_scan()" > /dev/null
cwait 3 "SELECT pg_is_in_recovery()" f 60 > /dev/null
out=$(cwait 0 "SELECT convert_from(gp_sql.directory_table_get('cdocs', '$S0'), 'UTF8') IS NOT NULL" t 60)
out2=$(cq 0 "SELECT count(*) FROM directory_table('cdocs') WHERE content IS NOT NULL")
[ -z "$out" ] && [ "$out2" = 7 ] && ok "a promoted mirror serves its primary's files" \
	|| notok "files after a failover" "$out / $out2"
cq 0 "SELECT gp_sql.remove_file('cdocs', '$S0')" > /dev/null
[ ! -e "$(cdir 3)/$CLOC/$S0" ] && ok "and removes one" || notok "a removal after a failover"
"$BINDIR/pg_ctl" -D "$(cdir 0)" -m fast -w stop > /dev/null 2>&1
for n in 2 3 4; do "$BINDIR/pg_ctl" -D "$(cdir "$n")" -m fast -w stop > /dev/null 2>&1; done

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
