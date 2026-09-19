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
# Storage servers: where a tablespace's files may live.
#
# Cloudberry keeps them in two shared catalogs whose columns are a foreign
# server's columns.  Here they are foreign servers, so most of what these
# tests ask is whether PostgreSQL's own rules now apply -- who may change a
# mapping, whose credentials are visible, and what pg_dump writes.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/storage/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-sto-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbs-XXXXXX)"
PORT="${PGPORT:-$((7100 + RANDOM % 200))}"
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

echo "storage servers: foreign servers of a data-less wrapper"
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
mkdir -p "$WORK/local_space" "$WORK/remote_space"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_sql CASCADE;")
case "$out" in
	*ERROR*) echo "the extension could not be created:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. a storage server is a foreign server of the gp_storage wrapper"
###############################################################################
is "the wrapper arrives with the extension, and reads nothing itself" \
   "SELECT fdwhandler = 0 AND fdwvalidator = 0
      FROM pg_foreign_data_wrapper WHERE fdwname = 'gp_storage';" "t"
isl "a server can be defined with options" \
   "SELECT gp_sql.create_storage_server('s3',
             '{\"endpoint\": \"s3.example.com\", \"region\": \"eu-west-1\"}');
    SELECT options::text FROM gp_sql.storage_servers WHERE servername = 's3';" \
   "{endpoint=s3.example.com,region=eu-west-1}"
is "and it belongs to whoever defined it" \
   "SELECT serverowner FROM gp_sql.storage_servers WHERE servername = 's3';" \
   "$(q 'SELECT CURRENT_USER;')"
is "an ordinary foreign server of another wrapper is not one" \
   "CREATE EXTENSION postgres_fdw;
    CREATE SERVER plain_fdw FOREIGN DATA WRAPPER postgres_fdw;
    SELECT count(*) FROM gp_sql.storage_servers WHERE servername = 'plain_fdw';" "0"

###############################################################################
echo "2. its options can be changed, added to and dropped"
###############################################################################
isl "an option that is there is set, and one that is not is added" \
   "SELECT gp_sql.alter_storage_server('s3',
             set_options => '{\"region\": \"eu-central-1\", \"bucket\": \"docs\"}');
    SELECT options::text FROM gp_sql.storage_servers WHERE servername = 's3';" \
   "{endpoint=s3.example.com,region=eu-central-1,bucket=docs}"
isl "and one can be dropped" \
   "SELECT gp_sql.alter_storage_server('s3', drop_options => ARRAY['bucket']);
    SELECT options::text FROM gp_sql.storage_servers WHERE servername = 's3';" \
   "{endpoint=s3.example.com,region=eu-central-1}"

###############################################################################
echo "3. credentials go where another user cannot read them"
###############################################################################
q "CREATE ROLE other LOGIN;" > /dev/null
isl "a user mapping carries them" \
   "SELECT gp_sql.create_storage_user_mapping('s3', CURRENT_USER,
             '{\"access_key\": \"AKIA\", \"secret\": \"shh\"}');
    SELECT options::text FROM gp_sql.storage_user_mappings WHERE servername = 's3';" \
   "{access_key=AKIA,secret=shh}"
out=$("$PSQL" -X -q -t -A -d postgres -U other \
	  -c "SELECT coalesce(options::text, 'hidden') FROM gp_sql.storage_user_mappings;" 2>&1)
[ "$out" = "hidden" ] && ok "and someone else sees that there is one, not what is in it" \
	|| notok "another user cannot read the credentials" "$out"
out=$("$PSQL" -X -q -t -A -d postgres -U other \
	  -c "SELECT gp_sql.create_storage_user_mapping('s3', 'postgres', '{\"secret\": \"mine\"}');" 2>&1)
case "$out" in
	*"must be owner of"*|*permission*) ok "nor define one for somebody else" ;;
	*) notok "nor define one for somebody else" "$out" ;;
esac
isl "a user may define their own" \
   "GRANT USAGE ON FOREIGN SERVER s3 TO other;
    SELECT gp_sql.create_storage_user_mapping('s3', 'other', '{\"secret\": \"theirs\"}');
    SELECT count(*) FROM gp_sql.storage_user_mappings WHERE servername = 's3';" "2"

###############################################################################
echo "4. a tablespace can say which storage server its files go through"
###############################################################################
q "CREATE TABLESPACE local_space LOCATION '$WORK/local_space';" > /dev/null
isl "an ordinary tablespace names none" \
   "SELECT coalesce(gp_sql.tablespace_storage_server('local_space'), 'none');" "none"
# CREATE TABLESPACE may not run inside a transaction block, so it goes in a
# statement of its own.
q "CREATE TABLESPACE remote_space LOCATION '$WORK/remote_space' WITH (gp.server = 's3');" > /dev/null
is "one made with gp.server names it" \
   "SELECT gp_sql.tablespace_storage_server('remote_space');" "s3"
is "and it is listed" \
   "SELECT tablespacename || ' -> ' || servername FROM gp_sql.storage_tablespaces;" \
   "remote_space -> s3"
q "SELECT gp_sql.create_storage_server('s3_backup');" > /dev/null
isl "ALTER TABLESPACE can change it" \
   "ALTER TABLESPACE remote_space SET (gp.server = 's3_backup');
    SELECT gp_sql.tablespace_storage_server('remote_space');" "s3_backup"
refused "an option in the namespace that is not one of ours is refused" \
        "ALTER TABLESPACE remote_space SET (gp.nonsense = 'x');" \
        "unrecognized tablespace option"
is "and the tablespace's own options still work" \
   "ALTER TABLESPACE local_space SET (seq_page_cost = 1.5);
    SELECT spcoptions::text FROM pg_tablespace WHERE spcname = 'local_space';" \
   "{seq_page_cost=1.5}"

###############################################################################
echo "5. what that means for a directory table today"
###############################################################################
isl "one in an ordinary tablespace is made as usual" \
   "SELECT gp_sql.create_directory_table('here', 'local_space');
    SELECT gp_sql.directory_table_location('here'::regclass) LIKE 'pg_tblspc/%';" "t"
refused "one that would reach a storage server is refused, not given local files" \
        "SELECT gp_sql.create_directory_table('there', 'remote_space');" \
        "cannot create a directory table in a tablespace that reaches storage server"
is "and no table is left behind" \
   "SELECT count(*) FROM pg_class WHERE relname = 'there';" "0"

###############################################################################
echo "6. the definitions are dumped, which Cloudberry's tools never did"
###############################################################################
"$BINDIR/pg_dump" -d postgres > "$WORK/dump.sql" 2>"$WORK/dump.err"
if grep -q "CREATE SERVER s3 FOREIGN DATA WRAPPER gp_storage" "$WORK/dump.sql"; then
	ok "pg_dump writes the server"
else
	notok "pg_dump writes the server" "$(grep -i 'CREATE SERVER' "$WORK/dump.sql" | head -3)"
fi
if grep -q "CREATE USER MAPPING FOR" "$WORK/dump.sql"; then
	ok "and the user mapping"
else
	notok "and the user mapping" "$(head -3 "$WORK/dump.err")"
fi
"$BINDIR/pg_dumpall" -g > "$WORK/dumpall.sql" 2>/dev/null
if grep -q "SECURITY LABEL FOR gp ON TABLESPACE remote_space IS 'storage_server=s3_backup'" "$WORK/dumpall.sql"; then
	ok "and pg_dumpall writes which storage server a tablespace reaches"
else
	notok "pg_dumpall writes the tablespace's storage server" \
	      "$(grep -i 'security label' "$WORK/dumpall.sql" | head -3)"
fi

###############################################################################
echo "7. dropping one"
###############################################################################
refused "a server in use by a mapping is not dropped by accident" \
        "SELECT gp_sql.drop_storage_server('s3');" "depend"
isl "dropping one nothing uses works" \
   "SELECT gp_sql.drop_storage_server('s3_backup');
    SELECT count(*) FROM gp_sql.storage_servers WHERE servername = 's3_backup';" "0"
refused "and dropping one that was never there says so" \
        "SELECT gp_sql.drop_storage_server('ghost');" "does not exist"
isl "unless it is asked not to" \
   "SELECT gp_sql.drop_storage_server('ghost', missing_ok => true);
    SELECT 'survived';" "survived"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
