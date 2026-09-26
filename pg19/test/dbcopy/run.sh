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
# A database copied or moved takes its modules' directories with it.
#
# PostgreSQL copies a database's directory without its subdirectories --
# CREATE DATABASE ... TEMPLATE, by either strategy, and ALTER DATABASE ...
# SET TABLESPACE, which then removes the old directory whole -- and a PAX
# table keeps its files in <relfilenode>_pax there, a directory table in
# <relid>_dirtable.  gp_core copies them as the statement runs
# (gp_dbcopy.c), and a standby copies its own as it replays the statement.
# What this checks: a copy reads as its template does and is a database of
# its own, a database moved keeps everything, a copy or a move that fails
# leaves nothing behind, a standby has the same files, and a directory table
# stays in its tablespace, whose directory its files are in.  And, while PAX
# tables are here, that PAX refuses a BRIN index.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/dbcopy/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/pax.control" ]; then
	echo "pax is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-dbcopy-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbc-XXXXXX)"
PORT="${PGPORT:-$((7300 + RANDOM % 200))}"
SBPORT=$((PORT + 1))
export PGPORT="$PORT" PGHOST="$SOCK"

pass=0; fail=0
ok()    { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok() { printf '  NOT OK %s\n' "$1"
          [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/         /'
          fail=$((fail + 1)); }

cleanup() {
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	"$BINDIR/pg_ctl" -D "$WORK/standby" -m immediate stop > /dev/null 2>&1
	chmod -R u+rwX "$WORK" 2> /dev/null
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

qd() { "$PSQL" -X -q -t -A -d "$1" -c "$2" 2>&1; }			# qd <database> <sql>
q()  { qd postgres "$1"; }
qs() { "$PSQL" -X -q -t -A -p "$SBPORT" -d "$1" -c "$2" 2>&1; }	# the standby

is() {						# is <what> <database> <sql> <want>
	local got; got=$(qd "$2" "$3")
	[ "$got" = "$4" ] && ok "$1" || notok "$1" "want [$4], got [$got]"
}

refused() {					# refused <what> <database> <sql> <part of the error>
	local got; got=$(qd "$2" "$3")
	case "$got" in
		*"$4"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$4], got [$got]" ;;
	esac
}

# The database directories of a tablespace, less those pg_database names.
strays() {
	local known
	known=$(q "SELECT oid FROM pg_database;")
	for d in "$WORK/data/base" "$WORK"/data/pg_tblspc/*/PG_*; do
		[ -d "$d" ] || continue
		for e in "$d"/*/; do
			e=$(basename "$e")
			[[ "$e" =~ ^[0-9]+$ ]] && ! grep -qx "$e" <<< "$known" && echo "$d/$e"
		done
	done
}

echo "a database copied or moved takes its modules' directories with it"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "shared_preload_libraries = 'gp_core,gp_sql,pax'"
	# the standby is on this machine: a tablespace each, in its data directory
	echo "allow_in_place_tablespaces = on"
} >> "$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

# A hot standby, streaming from the node from the start, so that it replays
# every statement below; checked at the end (section 6).
SB="$WORK/standby"
"$BINDIR/pg_basebackup" -D "$SB" -X stream -c fast -R > "$WORK/basebackup.log" 2>&1 &&
	echo "port = $SBPORT" >> "$SB/postgresql.conf" &&
	"$BINDIR/pg_ctl" -D "$SB" -l "$WORK/standby.log" -w -t 60 start > /dev/null 2>&1
caught_up() {				# until the standby has replayed what the node wrote
	local lsn
	lsn=$(q "SELECT pg_current_wal_insert_lsn();")
	for _ in $(seq 150); do
		[ "$(qs postgres "SELECT pg_last_wal_replay_lsn() >= '$lsn';")" = t ] && return 0
		sleep 0.2
	done
	return 1
}

out=$(q "CREATE TABLESPACE ts1 LOCATION '';"
      q "CREATE TABLESPACE ts2 LOCATION '';"
      q "CREATE DATABASE tmpl;")
out2=$(qd tmpl "CREATE EXTENSION gp_sql CASCADE; CREATE EXTENSION pax;" | grep -v NOTICE)
case "$out$out2" in
	*ERROR*) echo "the template could not be made:"
	         printf '%s\n' "$out" "$out2" | sed 's/^/  /'; exit 1 ;;
esac

# The template: a PAX table in its default tablespace and one in ts1, and a
# directory table in each.
qd tmpl "CREATE TABLE p (a int, b text) USING pax;
         INSERT INTO p SELECT i, 'row ' || i FROM generate_series(1, 1000) i;
         CREATE TABLE pt (a int) USING pax TABLESPACE ts1;
         INSERT INTO pt SELECT generate_series(1, 500);
         SELECT gp_sql.create_directory_table('docs');
         SELECT gp_sql.directory_table_put('docs'::regclass, 'a.txt', 'alpha'::bytea);
         SELECT gp_sql.directory_table_put('docs'::regclass, 'sub/b.txt', 'beta'::bytea);
         SELECT gp_sql.create_directory_table('docst', 'ts1');
         SELECT gp_sql.directory_table_put('docst'::regclass, 't.txt', 'tee'::bytea);" > /dev/null

# What a database of the template's tables reads: its rows and its files.
reads() {
	qd "$1" "SELECT (SELECT count(*) FROM p) || '/' || (SELECT count(*) FROM pt) || '/' ||
	               convert_from(gp_sql.directory_table_get('docs'::regclass, 'a.txt'), 'UTF8') || '/' ||
	               convert_from(gp_sql.directory_table_get('docs'::regclass, 'sub/b.txt'), 'UTF8') || '/' ||
	               convert_from(gp_sql.directory_table_get('docst'::regclass, 't.txt'), 'UTF8');"
}
WANT="1000/500/alpha/beta/tee"

# Where a database's directory is, relative to the data directory.
dbdir() {					# dbdir <database> <tablespace>
	local oid spc
	oid=$(q "SELECT oid FROM pg_database WHERE datname = '$1';")
	spc=$(q "SELECT oid FROM pg_tablespace WHERE spcname = '$2';")
	case "$2" in
		pg_default) echo "base/$oid" ;;
		*) echo "pg_tblspc/$spc/$(ls "$WORK/data/pg_tblspc/$spc/")/$oid" ;;
	esac
}

###############################################################################
echo "1. CREATE DATABASE ... TEMPLATE copies them, by either strategy"
###############################################################################
[ "$(reads tmpl)" = "$WANT" ] && ok "the template reads its tables' rows and files" \
	|| notok "the template" "$(reads tmpl)"

for strategy in wal_log file_copy; do
	out=$(q "CREATE DATABASE c_$strategy TEMPLATE tmpl STRATEGY $strategy;")
	got=$(reads "c_$strategy")
	[ -z "$out" ] && [ "$got" = "$WANT" ] \
		&& ok "a copy by $strategy reads what its template does: PAX's rows and the directory tables' files" \
		|| notok "a copy by $strategy" "$out / $got"
done

is "a copy's directory table is in the copy's own directory, not the template's" c_wal_log \
   "SELECT gp_sql.directory_table_location('docs'::regclass) = '$(dbdir c_wal_log pg_default)/' || 'docs'::regclass::oid || '_dirtable'
       AND gp_sql.directory_table_location('docst'::regclass) = '$(dbdir c_wal_log ts1)/' || 'docst'::regclass::oid || '_dirtable';" "t"

# Another default tablespace: what the template has in its own goes to the
# copy's, what it has in ts1 stays in ts1, as the core copies relations.
out=$(q "CREATE DATABASE c_ts2 TEMPLATE tmpl TABLESPACE ts2;")
pfn=$(qd c_ts2 "SELECT pg_relation_filenode('p');")
[ -z "$out" ] && [ "$(reads c_ts2)" = "$WANT" ] && [ -d "$WORK/data/$(dbdir c_ts2 ts2)/${pfn}_pax" ] \
	&& ok "a copy in another default tablespace has the template's default one's in its own" \
	|| notok "a copy in ts2" "$out / $(reads c_ts2) / $(ls "$WORK/data/$(dbdir c_ts2 ts2)" | grep -E '_pax|_dirtable' | tr '\n' ' ')"

###############################################################################
echo "2. a copy is a database of its own"
###############################################################################
qd c_wal_log "SELECT gp_sql.directory_table_put('docs'::regclass, 'new.txt', 'new'::bytea);
              SELECT gp_sql.remove_file('docs'::regclass, 'a.txt');
              INSERT INTO p SELECT generate_series(1, 10);" > /dev/null
is "what the copy writes and removes, it writes and removes in its own" c_wal_log \
   "SELECT (SELECT count(*) FROM p) || '/' || convert_from(gp_sql.directory_table_get('docs'::regclass, 'new.txt'), 'UTF8') || '/' ||
           (gp_sql.directory_table_get('docs'::regclass, 'a.txt') IS NULL);" "1010/new/true"
is "and the template has what it had" tmpl \
   "SELECT (SELECT count(*) FROM p) || '/' || (gp_sql.directory_table_get('docs'::regclass, 'new.txt') IS NULL) || '/' ||
           convert_from(gp_sql.directory_table_get('docs'::regclass, 'a.txt'), 'UTF8');" "1000/true/alpha"
out=$(q "DROP DATABASE c_wal_log;")
[ -z "$out" ] && [ "$(reads tmpl)" = "$WANT" ] \
	&& ok "DROP DATABASE of the copy leaves the template's files" \
	|| notok "DROP DATABASE of a copy" "$out / $(reads tmpl)"

###############################################################################
echo "3. ALTER DATABASE ... SET TABLESPACE moves them with the rest"
###############################################################################
q "CREATE DATABASE m TEMPLATE tmpl;" > /dev/null
old=$(dbdir m pg_default)
out=$(q "ALTER DATABASE m SET TABLESPACE ts2;")
[ -z "$out" ] && [ "$(reads m)" = "$WANT" ] && [ ! -e "$WORK/data/$old" ] \
	&& ok "a database moved reads its rows and files, and its old directory is gone" \
	|| notok "a database moved to ts2" "$out / $(reads m) / $(ls "$WORK/data/$old" 2>&1 | head -3)"
is "its directory table's files are in its directory of the new tablespace" m \
   "SELECT gp_sql.directory_table_location('docs'::regclass) = '$(dbdir m ts2)/' || 'docs'::regclass::oid || '_dirtable';" "t"
out=$(q "ALTER DATABASE m SET TABLESPACE pg_default;")
[ -z "$out" ] && [ "$(reads m)" = "$WANT" ] && [ ! -e "$WORK/data/$(dbdir m ts2)" ] \
	&& ok "and moved back, the same" \
	|| notok "a database moved back" "$out / $(reads m)"
# A move the core refuses: another session is in the database, still after
# the five seconds movedb() waits for it to leave.
"$PSQL" -X -q -d m -c "SELECT pg_sleep(8);" > /dev/null 2>&1 &
holder=$!
sleep 1
refused "a move the core refuses, another session being in the database, is refused" postgres \
	"ALTER DATABASE m SET TABLESPACE ts2;" 'database "m" is being accessed by other users'
wait "$holder"
[ -z "$(strays)" ] && [ "$(reads m)" = "$WANT" ] \
	&& ok "and copies nothing: the database is as it was, and nothing of it is in the tablespace" \
	|| notok "after a refused move" "$(strays) / $(reads m)"

###############################################################################
echo "4. a copy or a move that fails leaves nothing behind"
###############################################################################
# A file of the template's PAX table the server cannot read: the copy fails
# on it, after the core's own copy is done.
if [ "$(id -u)" -eq 0 ]; then
	ok "(skipped: root reads a file whatever its mode)"
else
	pdir="$WORK/data/$(qd tmpl "SELECT pg_relation_filepath('p');")_pax"
	pfile="$pdir/$(ls "$pdir" | head -1)"
	chmod 000 "$pfile"
	refused "a copy that cannot read a PAX file fails" postgres \
		"CREATE DATABASE bad TEMPLATE tmpl;" "Permission denied"
	is "and there is no such database" postgres "SELECT count(*) FROM pg_database WHERE datname = 'bad';" "0"
	[ -z "$(strays)" ] && ok "nor a directory of it, in any tablespace" \
		|| notok "the failed copy's directories" "$(strays)"
	chmod 600 "$pfile"
	out=$(q "CREATE DATABASE bad TEMPLATE tmpl;")
	[ -z "$out" ] && [ "$(reads bad)" = "$WANT" ] && ok "with the file readable again, the copy is made" \
		|| notok "the copy made again" "$out / $(reads bad)"
	q "DROP DATABASE bad;" > /dev/null

	pdir="$WORK/data/$(qd m "SELECT pg_relation_filepath('p');")_pax"
	pfile="$pdir/$(ls "$pdir" | head -1)"
	chmod 000 "$pfile"
	refused "a move that cannot read a PAX file fails" postgres \
		"ALTER DATABASE m SET TABLESPACE ts2;" "Permission denied"
	chmod 600 "$pfile"
	is "and the database stays where it was" postgres \
		"SELECT spcname FROM pg_database d JOIN pg_tablespace t ON t.oid = d.dattablespace WHERE datname = 'm';" "pg_default"
	[ -z "$(strays)" ] && [ "$(reads m)" = "$WANT" ] \
		&& ok "with nothing of it in the tablespace it was going to, and everything where it is" \
		|| notok "after a failed move" "$(strays) / $(reads m)"
fi

###############################################################################
echo "5. a directory table stays in its tablespace, as Cloudberry's does"
###############################################################################
# Its files are in its database's directory of its tablespace, and would
# stay behind.  Cloudberry refuses the first, and passes a directory table
# over in the second.
refused "ALTER TABLE ... SET TABLESPACE of a directory table is refused, in Cloudberry's words" tmpl \
	"ALTER TABLE docs SET TABLESPACE ts2;" \
	'ALTER action SET TABLESPACE cannot be performed on relation "docs"'
refused "and ALTER TABLE ALL IN TABLESPACE of the tablespace one is in" tmpl \
	"ALTER TABLE ALL IN TABLESPACE ts1 SET TABLESPACE ts2;" \
	'cannot move directory table "docst" to another tablespace'
qd tmpl "CREATE ROLE mover; CREATE TABLE moved (a int) TABLESPACE ts1; ALTER TABLE moved OWNER TO mover;" > /dev/null
is "which moves the tables of a role that has no directory table" tmpl \
   "ALTER TABLE ALL IN TABLESPACE ts1 OWNED BY mover SET TABLESPACE ts2;
    SELECT spcname FROM pg_class c JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE relname = 'moved';" "ts2"
is "and an ordinary table still moves" tmpl \
   "ALTER TABLE moved SET TABLESPACE ts1;
    SELECT spcname FROM pg_class c JOIN pg_tablespace t ON t.oid = c.reltablespace WHERE relname = 'moved';" "ts1"
qd tmpl "DROP TABLE moved; DROP ROLE mover;" > /dev/null

###############################################################################
echo "6. a standby does the same, with its own copy of the source"
###############################################################################
[ "$(qs postgres "SELECT pg_is_in_recovery();")" = t ] && ok "a hot standby, streaming from the start" \
	|| notok "a hot standby" "$(tail -n 3 "$WORK/basebackup.log" "$WORK/standby.log")"
caught_up || notok "the standby catches up"
same=1; compared=0
for db in tmpl c_file_copy c_ts2 m; do
	for spc in pg_default ts1 ts2; do
		d=$(dbdir "$db" "$spc")
		[ -d "$WORK/data/$d" ] || continue
		for e in "$WORK/data/$d"/*_pax "$WORK/data/$d"/*_dirtable; do
			[ -d "$e" ] || continue
			compared=$((compared + 1))
			diff -r "$e" "$SB/$d/$(basename "$e")" > /dev/null 2>&1 || { same=0; echo "         $d/$(basename "$e")"; }
		done
	done
done
# four databases, each with two PAX tables' directories and two directory tables'
[ "$same" -eq 1 ] && [ "$compared" -eq 16 ] \
	&& ok "the standby's copies and moved directories are the node's, file for file" \
	|| notok "the standby's directories differ from the node's (above)" "$compared compared"
out=$(for db in c_file_copy c_ts2 m; do
	qs "$db" "SELECT (SELECT count(*) FROM p) || '/' || (SELECT count(*) FROM pt) || '/' ||
	               convert_from(gp_sql.directory_table_get('docs'::regclass, 'a.txt'), 'UTF8') || '/' ||
	               convert_from(gp_sql.directory_table_get('docs'::regclass, 'sub/b.txt'), 'UTF8') || '/' ||
	               convert_from(gp_sql.directory_table_get('docst'::regclass, 't.txt'), 'UTF8');"
done | sort -u)
[ "$out" = "$WANT" ] && ok "and it reads them as the node does" || notok "reading the copies on the standby" "$out"
"$BINDIR/pg_ctl" -D "$SB" -m fast -w stop > /dev/null 2>&1

###############################################################################
echo "7. PAX refuses a BRIN index"
###############################################################################
# BRIN walks a table's block numbers, and a PAX row's has its file's number
# in the high bits; PAX gives BRIN no runs of them to walk (O18), and
# refuses the build, as Cloudberry's does, whatever makes the index.
refused "CREATE INDEX ... USING brin on a PAX table" tmpl \
	"CREATE INDEX ON p USING brin (a);" "pax only support btree/hash/gin/bitmap indexes"
refused "and a heap table with one made PAX" tmpl \
	"CREATE TABLE hb (a int); CREATE INDEX ON hb USING brin (a); ALTER TABLE hb SET ACCESS METHOD pax;" \
	"pax only support btree/hash/gin/bitmap indexes"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
