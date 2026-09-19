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

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
