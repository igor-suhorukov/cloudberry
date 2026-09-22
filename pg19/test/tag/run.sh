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
# Tags: metadata a user hangs on an object.
#
# Cloudberry writes CREATE TAG and then TAG (name = 'value') on eleven kinds
# of statement, and keeps both in shared catalogs.  Here the definitions are
# an ordinary table and the assignments are "gp_tag" security labels, so what
# these tests ask is whether the same things can be said, and whether what
# PostgreSQL gives for free -- dropping a label with its object, and dumping
# it -- actually happens.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/tag/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-tag-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbt-XXXXXX)"
PORT="${PGPORT:-$((6700 + RANDOM % 200))}"
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

# isl <label> <sql> <expected>	the answer is the script's last non-empty line
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

echo "tags: definitions in a table, assignments in a security label"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	# gp_sql reads CREATE TABLE before PostgreSQL does and registers the
	# "gp_tag" label provider, so it has to be preloaded.
	echo "shared_preload_libraries = 'gp_core,gp_sql'"
} >> "$WORK/data/postgresql.conf"
mkdir -p "$WORK/tblspc"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_sql CASCADE;")
case "$out" in
	*ERROR*) echo "the extension could not be created:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. a tag is defined, and then an object carries it"
###############################################################################
isl "a tag can be defined" \
   "SELECT gp_sql.create_tag('env');
    SELECT count(*) FROM gp_sql.tag WHERE tagname = 'env';" "1"
is "and it belongs to whoever defined it" \
   "SELECT tagowner::regrole::text FROM gp_sql.tag WHERE tagname = 'env';" \
   "$(q 'SELECT CURRENT_USER;')"
q "CREATE TABLE t (id int);" > /dev/null
isl "a table can be tagged" \
   "SELECT gp_sql.set_relation_tag('t'::regclass, 'env', 'prod');
    SELECT gp_sql.relation_tags('t'::regclass)::text;" '{"env": "prod"}'
is "the tag is a security label, which is where it can be dropped and dumped from" \
   "SELECT label FROM pg_seclabel
     WHERE objoid = 't'::regclass AND provider = 'gp_tag';" '{"env": "prod"}'
is "and it is listed the way Cloudberry lists it" \
   "SELECT relname || ' ' || tagname || '=' || tagvalue
      FROM gp_sql.relation_tag_descriptions WHERE relname = 't';" "t env=prod"

###############################################################################
echo "2. a second tag joins the first rather than replacing it"
###############################################################################
isl "two tags on one object" \
   "SELECT gp_sql.create_tag('team');
    SELECT gp_sql.set_relation_tag('t'::regclass, 'team', 'data');
    SELECT gp_sql.relation_tags('t'::regclass)::text;" \
   '{"env": "prod", "team": "data"}'
isl "one can be changed on its own" \
   "SELECT gp_sql.set_relation_tag('t'::regclass, 'env', 'staging');
    SELECT gp_sql.relation_tags('t'::regclass)::text;" \
   '{"env": "staging", "team": "data"}'
isl "and one can be taken away" \
   "SELECT gp_sql.unset_relation_tag('t'::regclass, 'env');
    SELECT gp_sql.relation_tags('t'::regclass)::text;" '{"team": "data"}'
isl "the label goes when the last tag does" \
   "SELECT gp_sql.unset_relation_tag('t'::regclass, 'team');
    SELECT count(*) FROM pg_seclabel
      WHERE objoid = 't'::regclass AND provider = 'gp_tag';" "0"

###############################################################################
echo "3. allowed values are what the definition says"
###############################################################################
q "SELECT gp_sql.create_tag('tier', ARRAY['gold','silver']);" > /dev/null
isl "a value on the list is accepted" \
   "SELECT gp_sql.set_relation_tag('t'::regclass, 'tier', 'gold');
    SELECT gp_sql.relation_tags('t'::regclass)->>'tier';" "gold"
refused "a value that is not is refused" \
        "SELECT gp_sql.set_relation_tag('t'::regclass, 'tier', 'bronze');" \
        "is not in tag \"tier\" allowed values"
refused "and so is a tag nobody defined" \
        "SELECT gp_sql.set_relation_tag('t'::regclass, 'nope', 'x');" \
        "tag \"nope\" does not exist"
isl "a value can be added to the list" \
   "SELECT gp_sql.alter_tag('tier', add_values => ARRAY['bronze']);
    SELECT gp_sql.set_relation_tag('t'::regclass, 'tier', 'bronze');
    SELECT gp_sql.relation_tags('t'::regclass)->>'tier';" "bronze"
refused "a value in use cannot be dropped from the list" \
        "SELECT gp_sql.alter_tag('tier', drop_values => ARRAY['bronze']);" \
        "which is in use"
isl "unsetting the list lets any value through" \
   "SELECT gp_sql.alter_tag('tier', unset_values => true);
    SELECT gp_sql.set_relation_tag('t'::regclass, 'tier', 'anything');
    SELECT gp_sql.relation_tags('t'::regclass)->>'tier';" "anything"

# Cloudberry's rules for the list (tag.c, transformTagValues), which its own
# tag test holds the port to: each value once, none of more than 256 bytes,
# no more than 300, and none dropped that is not there.  Adding and dropping
# were once a union and a difference, and answered where Cloudberry refuses.
refused "a value already on the list is refused, not added again" \
        "SELECT gp_sql.alter_tag('tier', add_values => ARRAY['gold']);
         SELECT gp_sql.alter_tag('tier', add_values => ARRAY['gold']);" \
        'allowed value "gold" has been added'
refused "and so is one given twice" \
        "SELECT gp_sql.create_tag('twice', ARRAY['a', 'a']);" \
        'allowed value "a" has been added'
refused "or one of more than 256 bytes" \
        "SELECT gp_sql.create_tag('long', ARRAY[repeat('x', 257)]);" \
        "has exceeded max 256 length"
refused "or a 301st" \
        "SELECT gp_sql.create_tag('many', (SELECT array_agg(g::text) FROM generate_series(1, 301) g));" \
        "Allowed_values only allow 300 values."
refused "and dropping one that is not there is refused" \
        "SELECT gp_sql.alter_tag('tier', drop_values => ARRAY['platinum']);" \
        'allowed value "platinum" not found'
isl "a value added goes after the ones there, as Cloudberry keeps them" \
   "SELECT gp_sql.create_tag('order_kept', ARRAY['z', 'a']);
    SELECT gp_sql.alter_tag('order_kept', add_values => ARRAY['m']);
    SELECT array_to_string(allowed_values, ',') FROM gp_sql.tag WHERE tagname = 'order_kept';" "z,a,m"

###############################################################################
echo "4. the WITH (gp_tag.x = 'y') shorthand, which is Cloudberry's TAG clause"
###############################################################################
isl "on CREATE TABLE" \
   "CREATE TABLE wt (id int) WITH (gp_tag.env = 'prod', fillfactor = 70);
    SELECT gp_sql.relation_tags('wt'::regclass)::text;" '{"env": "prod"}'
is "and the options PostgreSQL knows are left alone" \
   "SELECT reloptions::text FROM pg_class WHERE relname = 'wt';" "{fillfactor=70}"
isl "on CREATE VIEW" \
   "CREATE VIEW wv WITH (gp_tag.env = 'prod') AS SELECT 1 AS x;
    SELECT gp_sql.relation_tags('wv'::regclass)::text;" '{"env": "prod"}'
isl "on CREATE MATERIALIZED VIEW" \
   "CREATE MATERIALIZED VIEW wm WITH (gp_tag.team = 'data') AS SELECT 1 AS x;
    SELECT gp_sql.relation_tags('wm'::regclass)::text;" '{"team": "data"}'
isl "on CREATE INDEX, whose name it does not have to be told" \
   "CREATE INDEX ON wt (id) WITH (gp_tag.env = 'prod');
    SELECT gp_sql.relation_tags('wt_id_idx'::regclass)::text;" '{"env": "prod"}'
refused "a tag nobody defined is refused before the table is made" \
        "CREATE TABLE never (id int) WITH (gp_tag.nope = 'x');" \
        "tag \"nope\" does not exist"
is "and that table is not there" \
   "SELECT count(*) FROM pg_class WHERE relname = 'never';" "0"

###############################################################################
echo "5. ALTER TABLE SET and RESET, which are ALTER ... TAG and UNSET TAG"
###############################################################################
isl "SET adds one" \
   "ALTER TABLE wt SET (gp_tag.team = 'data');
    SELECT gp_sql.relation_tags('wt'::regclass)::text;" \
   '{"env": "prod", "team": "data"}'
isl "RESET takes one away" \
   "ALTER TABLE wt RESET (gp_tag.env);
    SELECT gp_sql.relation_tags('wt'::regclass)::text;" '{"team": "data"}'
isl "SET beside an option PostgreSQL knows changes both" \
   "ALTER TABLE wt SET (gp_tag.env = 'qa', fillfactor = 60);
    SELECT gp_sql.relation_tags('wt'::regclass)->>'env'
           || ' ' || (SELECT reloptions::text FROM pg_class WHERE relname = 'wt');" \
   "qa {fillfactor=60}"

###############################################################################
echo "6. an index keeps its tags in a table, because a label cannot reach one"
###############################################################################
is "they are rows, not labels" \
   "SELECT count(*) FROM gp_sql.index_tag WHERE indexrelid = 'wt_id_idx'::regclass;" "1"
is "and they are listed with everything else" \
   "SELECT tagname || '=' || tagvalue FROM gp_sql.relation_tag_descriptions
     WHERE relname = 'wt_id_idx';" "env=prod"
isl "dropping the index forgets them" \
   "DROP INDEX wt_id_idx;
    SELECT count(*) FROM gp_sql.index_tag;" "0"

###############################################################################
echo "7. every other kind of object Cloudberry can tag"
###############################################################################
q "CREATE ROLE tagged_role; CREATE SCHEMA tagged_schema;" > /dev/null
q "CREATE TABLESPACE tagged_space LOCATION '$WORK/tblspc';" > /dev/null
isl "a role" \
   "SELECT gp_sql.set_role_tag('tagged_role'::regrole, 'env', 'prod');
    SELECT gp_sql.role_tags('tagged_role'::regrole)::text;" '{"env": "prod"}'
is "which is a shared label, so every database sees it" \
   "SELECT count(*) FROM pg_shseclabel
     WHERE objoid = 'tagged_role'::regrole AND provider = 'gp_tag';" "1"
isl "a schema" \
   "SELECT gp_sql.set_schema_tag('tagged_schema'::regnamespace, 'env', 'prod');
    SELECT gp_sql.schema_tags('tagged_schema'::regnamespace)::text;" '{"env": "prod"}'
isl "a database" \
   "SELECT gp_sql.set_database_tag('postgres', 'env', 'prod');
    SELECT gp_sql.database_tags('postgres')::text;" '{"env": "prod"}'
isl "a tablespace" \
   "SELECT gp_sql.set_tablespace_tag('tagged_space', 'env', 'prod');
    SELECT gp_sql.tablespace_tags('tagged_space')::text;" '{"env": "prod"}'
is "and each is listed under the name Cloudberry gives that view" \
   "SELECT (SELECT count(*) FROM gp_sql.user_tag_descriptions WHERE rolname = 'tagged_role')
         + (SELECT count(*) FROM gp_sql.schema_tag_descriptions WHERE nspname = 'tagged_schema')
         + (SELECT count(*) FROM gp_sql.database_tag_descriptions WHERE datname = 'postgres')
         + (SELECT count(*) FROM gp_sql.tablespace_tag_descriptions WHERE spcname = 'tagged_space');" \
   "4"

###############################################################################
echo "8. a tag is dropped with the object that carries it"
###############################################################################
isl "dropping a table takes its label with it" \
   "DROP TABLE wt;
    SELECT count(*) FROM pg_seclabel WHERE provider = 'gp_tag'
      AND classoid = 'pg_class'::regclass
      AND objoid NOT IN (SELECT oid FROM pg_class);" "0"
isl "and dropping a role takes its shared label" \
   "DROP ROLE tagged_role;
    SELECT count(*) FROM pg_shseclabel WHERE provider = 'gp_tag'
      AND classoid = 'pg_authid'::regclass;" "0"

###############################################################################
echo "9. a definition cannot be dropped or renamed out from under an object"
###############################################################################
refused "DROP TAG is refused while something carries it" \
        "SELECT gp_sql.drop_tag('tier');" "object(s) carry it"
refused "and so is a rename" \
        "SELECT gp_sql.rename_tag('tier', 'level');" "object(s) carry it"
isl "once nothing does, it can be dropped" \
   "SELECT gp_sql.unset_relation_tag('t'::regclass, 'tier');
    SELECT gp_sql.drop_tag('tier');
    SELECT count(*) FROM gp_sql.tag WHERE tagname = 'tier';" "0"
refused "dropping one that was never there says so" \
        "SELECT gp_sql.drop_tag('ghost');" "tag \"ghost\" does not exist"
isl "unless it is asked not to" \
   "SELECT gp_sql.drop_tag('ghost', missing_ok => true);
    SELECT 'survived';" "survived"

###############################################################################
echo "10. the label itself is checked, however it is written"
###############################################################################
refused "a label that is not a JSON object is refused" \
        "SECURITY LABEL FOR gp_tag ON TABLE t IS '\"prod\"';" \
        "must be a JSON object"
refused "a tag nobody defined is refused" \
        "SECURITY LABEL FOR gp_tag ON TABLE t IS '{\"nope\": \"x\"}';" \
        "tag \"nope\" does not exist"
refused "and a value that is not a string" \
        "SECURITY LABEL FOR gp_tag ON TABLE t IS '{\"env\": 3}';" \
        "must be given a string value"
isl "a whole set can be written at once" \
   "SECURITY LABEL FOR gp_tag ON TABLE t IS '{\"env\": \"prod\", \"team\": \"data\"}';
    SELECT gp_sql.relation_tags('t'::regclass)::text;" \
   '{"env": "prod", "team": "data"}'

###############################################################################
echo "11. tagging is for the owner of the object"
###############################################################################
q "CREATE ROLE other LOGIN;" > /dev/null
out=$("$PSQL" -X -q -t -A -d postgres -U other \
	  -c "SELECT gp_sql.set_relation_tag('t'::regclass, 'env', 'theirs');" 2>&1)
case "$out" in
	*"must be owner"*) ok "someone else cannot tag your table" ;;
	*) notok "someone else cannot tag your table" "$out" ;;
esac
out=$("$PSQL" -X -q -t -A -d postgres -U other \
	  -c "UPDATE gp_sql.tag SET allowed_values = ARRAY['x'] WHERE tagname = 'env';
	      SELECT count(*) FROM gp_sql.tag WHERE tagname = 'env' AND allowed_values IS NOT NULL;" 2>&1 | tail -1)
[ "$out" = "0" ] && ok "nor change a definition they do not own" \
	|| notok "nor change a definition they do not own" "$out"

###############################################################################
echo "12. a dump carries the tags, which Cloudberry's own tools never did"
###############################################################################
"$BINDIR/pg_dump" -d postgres > "$WORK/dump.sql" 2>"$WORK/dump.err"
if grep -q "SECURITY LABEL FOR gp_tag ON TABLE public.t IS '{\"env\": \"prod\", \"team\": \"data\"}'" "$WORK/dump.sql"; then
	ok "pg_dump writes the assignment beside its table"
else
	notok "pg_dump writes the assignment beside its table" \
	      "$(grep -i 'security label' "$WORK/dump.sql" | head -3)$(head -3 "$WORK/dump.err")"
fi
if grep -q "COPY gp_sql.tag " "$WORK/dump.sql"; then
	ok "and the definitions, which are extension configuration tables"
else
	notok "and the definitions" "$(grep -n 'gp_sql' "$WORK/dump.sql" | head -5)"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
