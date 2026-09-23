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
# O26: Cloudberry's SQL, written the way Cloudberry writes it.
#
# The hook rewrites a statement into PostgreSQL's own spelling before the
# grammar sees it, so these tests write Cloudberry's DDL and then ask the
# port's own functions whether the right thing happened.  The other half of
# the suite is the opposite question: that a statement with none of
# Cloudberry's syntax in it comes through untouched, even when it holds one of
# the words that starts a rewrite.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/grammar/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-gram-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbg-XXXXXX)"
PORT="${PGPORT:-$((7500 + RANDOM % 200))}"
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

# at <name> <one-line statement> <error> <token>: the statement fails with that
# error, and psql's caret is under the token -- its first occurrence in the
# statement, or the one after "@@" if the token is written "@@token" there.
# psql puts the caret at the position the server reports, counted in the text
# it sent, which is what the rewrite has to report in.  A line longer than
# psql shows is cut, with "..." where it was, so where the caret is in the
# statement is where the shown part is in it plus where the caret is in that.
at() {
	local sql="${2//@@/}" got line shown caret pos want
	local before="${2%%@@*}"
	[ "$before" = "$2" ] && before="${sql%%"$4"*}"
	want=${#before}
	got=$(q "$sql")
	case "$got" in
		*"$3"*) ;;
		*) notok "$1" "expected an error containing [$3], got [$got]"; return ;;
	esac
	line=$(printf '%s\n' "$got" | grep -m1 '^LINE 1: ')
	caret=$(printf '%s\n' "$got" | grep -m1 '^ *\^$')
	shown=${line#LINE 1: }
	pos=$(( ${#caret} - 1 - 8 ))
	if [ "${shown#...}" != "$shown" ]; then
		shown=${shown#...}
		pos=$(( pos - 3 ))
	fi
	shown=${shown%...}
	before="${sql%%"$shown"*}"
	pos=$(( pos + ${#before} ))
	if [ -z "$line" ] || [ "$pos" != "$want" ]; then
		notok "$1" "caret at $pos, want $want ($4): $got"
		return
	fi
	ok "$1"
}

echo "O26: Cloudberry's SQL, rewritten into PostgreSQL 19's"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	# pg_stat_statements for section 13, which reads the statement texts it
	# cuts out by the offsets the parse tree carries.
	echo "shared_preload_libraries = 'gp_core,gp_task,gp_matview,gp_sql,gp_security,pg_stat_statements'"
	echo "gp.enable_password_profile = on"
	echo "max_worker_processes = 16"
} >> "$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_sql CASCADE;
         CREATE EXTENSION gp_task CASCADE;
         CREATE EXTENSION gp_matview CASCADE;
         CREATE EXTENSION gp_security CASCADE;")
case "$out" in
	*ERROR*) echo "the extensions could not be created:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. a statement with none of Cloudberry's syntax is left alone"
###############################################################################
is "an ordinary one still works" \
   "CREATE TABLE plain (id int, name text); INSERT INTO plain VALUES (1, 'x');
    SELECT count(*) FROM plain;" "1"
is "a table may be called tag, which is not a TAG clause" \
   "CREATE TABLE tag (id int); SELECT count(*) FROM tag;" "0"
is "and a column may be" \
   "CREATE TABLE has_tag (tag text, distributed int); SELECT count(*) FROM has_tag;" "0"
is "a function call in a query is not one either" \
   "CREATE FUNCTION tag(int) RETURNS int LANGUAGE sql AS 'SELECT \$1';
    CREATE TABLE ctas AS SELECT tag(1) AS x;
    SELECT x FROM ctas;" "1"
is "a string that reads like one is just a string" \
   "SELECT 'CREATE TAG x ALLOWED_VALUES ''a''' = 'CREATE TAG x ALLOWED_VALUES ''a''';" "t"
is "and so is a comment" \
   "/* CREATE TAG commented_out */ SELECT 1;" "1"
is "dollar quoting is the scanner's, so a body is untouched" \
   "CREATE FUNCTION body() RETURNS text LANGUAGE sql AS \$\$ SELECT 'DISTRIBUTED BY (a)' \$\$;
    SELECT body();" "DISTRIBUTED BY (a)"

###############################################################################
echo "2. CREATE TAG, ALTER TAG, DROP TAG"
###############################################################################
isl "CREATE TAG" \
   "CREATE TAG env;
    SELECT count(*) FROM gp_sql.tag WHERE tagname = 'env';" "1"
isl "CREATE TAG ... ALLOWED_VALUES" \
   "CREATE TAG tier ALLOWED_VALUES 'gold', 'silver';
    SELECT array_to_string(allowed_values, ',') FROM gp_sql.tag WHERE tagname = 'tier';" \
   "gold,silver"
isl "CREATE TAG IF NOT EXISTS says nothing the second time" \
   "CREATE TAG IF NOT EXISTS env;
    SELECT count(*) FROM gp_sql.tag WHERE tagname = 'env';" "1"
isl "ALTER TAG ... ADD ALLOWED_VALUES" \
   "ALTER TAG tier ADD ALLOWED_VALUES 'bronze';
    SELECT array_length(allowed_values, 1) FROM gp_sql.tag WHERE tagname = 'tier';" "3"
isl "ALTER TAG ... DROP ALLOWED_VALUES" \
   "ALTER TAG tier DROP ALLOWED_VALUES 'bronze';
    SELECT array_length(allowed_values, 1) FROM gp_sql.tag WHERE tagname = 'tier';" "2"
isl "ALTER TAG ... UNSET ALLOWED_VALUES" \
   "ALTER TAG tier UNSET ALLOWED_VALUES;
    SELECT allowed_values IS NULL FROM gp_sql.tag WHERE tagname = 'tier';" "t"
isl "DROP TAG, more than one at a time" \
   "CREATE TAG t1; CREATE TAG t2; DROP TAG t1, t2;
    SELECT count(*) FROM gp_sql.tag WHERE tagname IN ('t1', 't2');" "0"
isl "DROP TAG IF EXISTS" \
   "DROP TAG IF EXISTS never_defined;
    SELECT 'survived';" "survived"
isl "ALTER TAG ... RENAME TO" \
   "CREATE TAG to_rename; ALTER TAG to_rename RENAME TO renamed;
    SELECT count(*) FROM gp_sql.tag WHERE tagname = 'renamed';" "1"
# Cloudberry's tag test writes ALTER TAG IF EXISTS, which the rewrite used to
# read past, so that a tag that was not there was an error.
refused "ALTER TAG IF EXISTS of one that is not there says so and does nothing" \
        "ALTER TAG IF EXISTS never_defined ADD ALLOWED_VALUES 'x';" \
        'NOTICE:  tag "never_defined" does not exist, skipping'
refused "and so does its RENAME" \
        "ALTER TAG IF EXISTS never_defined RENAME TO other;" \
        'NOTICE:  tag "never_defined" does not exist, skipping'

###############################################################################
echo "3. the TAG clause, on each kind of thing Cloudberry lets it be written on"
###############################################################################
isl "on CREATE TABLE" \
   "CREATE TABLE tagged (id int) TAG (env = 'prod', tier = 'gold');
    SELECT gp_sql.relation_tags('tagged'::regclass)::text;" \
   '{"env": "prod", "tier": "gold"}'
isl "on CREATE VIEW" \
   "CREATE VIEW tagged_v AS SELECT 1 AS x;
    ALTER VIEW tagged_v TAG (env = 'prod');
    SELECT gp_sql.relation_tags('tagged_v'::regclass)::text;" '{"env": "prod"}'
# Cloudberry's CREATE SCHEMA takes WITH TAG (...), and nothing else: its tag
# test expects a bare TAG there to be a syntax error, which it is, left for
# PostgreSQL's grammar to find.
isl "on CREATE SCHEMA, as WITH TAG" \
   "CREATE SCHEMA tagged_s WITH TAG (env = 'prod');
    SELECT gp_sql.schema_tags('tagged_s'::regnamespace)::text;" '{"env": "prod"}'
refused "and a bare TAG there is Cloudberry's syntax error" \
        "CREATE SCHEMA tagged_s2 TAG (env = 'prod');" 'syntax error at or near "TAG"'
# CREATE DATABASE and CREATE TABLESPACE may not run in a transaction block,
# and PostgreSQL runs a string of statements as one, so a statement followed
# by the call that tags it was refused.  Their tags go into the statement.
q "CREATE DATABASE tagged_db TAG (env = 'prod', tier = 'gold');" > /dev/null
is "on CREATE DATABASE, as options gp_sql takes out again" \
   "SELECT gp_sql.database_tags('tagged_db')::text;" '{"env": "prod", "tier": "gold"}'
refused "an undefined one is refused before the database is made" \
        "CREATE DATABASE never_db TAG (nope = 'x');" 'tag "nope" does not exist'
is "and none is" "SELECT count(*) FROM pg_database WHERE datname = 'never_db';" "0"
mkdir -p "$WORK/ts1" "$WORK/ts2"
q "CREATE TABLESPACE tagged_ts LOCATION '$WORK/ts1' TAG (env = 'prod');" > /dev/null
is "on CREATE TABLESPACE" \
   "SELECT gp_sql.tablespace_tags('tagged_ts')::text;" '{"env": "prod"}'
q "CREATE TABLESPACE tagged_ts2 LOCATION '$WORK/ts2' WITH (random_page_cost = 3.0) TAG (env = 'prod');" > /dev/null
is "and on one with a WITH list of its own, which keeps it" \
   "SELECT gp_sql.tablespace_tags('tagged_ts2')::text || ' ' || array_to_string(spcoptions, ',')
      FROM pg_tablespace WHERE spcname = 'tagged_ts2';" '{"env": "prod"} random_page_cost=3.0'
isl "on CREATE USER" \
   "CREATE USER tagged_u TAG (env = 'prod');
    SELECT gp_sql.role_tags('tagged_u'::regrole)::text;" '{"env": "prod"}'
isl "and on CREATE USER with options of PostgreSQL's beside it" \
   "CREATE USER tagged_u2 LOGIN CONNECTION LIMIT 10 TAG (env = 'prod');
    SELECT gp_sql.role_tags('tagged_u2'::regrole)::text || ' ' || rolconnlimit || ' ' || rolcanlogin
      FROM pg_roles WHERE rolname = 'tagged_u2';" '{"env": "prod"} 10 true'
isl "on CREATE SEQUENCE" \
   "CREATE SEQUENCE tagged_seq START 5 TAG (env = 'prod');
    SELECT gp_sql.relation_tags('tagged_seq'::regclass)::text || ' ' || nextval('tagged_seq');" \
   '{"env": "prod"} 5'
isl "and CREATE SEQUENCE IF NOT EXISTS of one that is there leaves its tags alone" \
   "CREATE SEQUENCE IF NOT EXISTS tagged_seq TAG (env = 'dev');
    SELECT gp_sql.relation_tags('tagged_seq'::regclass) ->> 'env';" "prod"
isl "and CREATE SCHEMA IF NOT EXISTS the same" \
   "CREATE SCHEMA IF NOT EXISTS tagged_s WITH TAG (env = 'dev');
    SELECT gp_sql.schema_tags('tagged_s'::regnamespace) ->> 'env';" "prod"
q "CREATE FOREIGN DATA WRAPPER tag_fdw; CREATE SERVER tag_srv FOREIGN DATA WRAPPER tag_fdw;" > /dev/null
isl "on CREATE FOREIGN TABLE, in its OPTIONS, beside the wrapper's own" \
   "CREATE FOREIGN TABLE tagged_ft (a int) SERVER tag_srv OPTIONS (path 'x') TAG (env = 'prod');
    SELECT gp_sql.relation_tags('tagged_ft'::regclass)::text || ' ' || array_to_string(ftoptions, ',')
      FROM pg_foreign_table WHERE ftrelid = 'tagged_ft'::regclass;" '{"env": "prod"} path=x'
isl "UNSET TAG takes one off" \
   "ALTER TABLE tagged UNSET TAG (tier);
    SELECT gp_sql.relation_tags('tagged'::regclass)::text;" '{"env": "prod"}'
refused "a tag nobody defined is still refused" \
        "CREATE TABLE never_made (id int) TAG (nope = 'x');" \
        "tag \"nope\" does not exist"
refused "and before a schema is made" \
        "CREATE SCHEMA never_s WITH TAG (nope = 'x');" "tag \"nope\" does not exist"
is "which it is not" "SELECT count(*) FROM pg_namespace WHERE nspname = 'never_s';" "0"
refused "or a role" \
        "CREATE USER never_u TAG (nope = 'x');" "tag \"nope\" does not exist"
is "which it is not either" "SELECT count(*) FROM pg_roles WHERE rolname = 'never_u';" "0"

# ALTER of each kind of object, whose TAG and UNSET TAG are whole statements
# of Cloudberry's: each the ALTER PostgreSQL has for that kind of object, or,
# where it has none that takes an option -- a schema -- a CALL.
isl "ALTER SCHEMA ... TAG" \
   "ALTER SCHEMA tagged_s TAG (tier = 'gold');
    SELECT gp_sql.schema_tags('tagged_s'::regnamespace)::text;" '{"env": "prod", "tier": "gold"}'
isl "and UNSET TAG" \
   "ALTER SCHEMA tagged_s UNSET TAG (env, tier);
    SELECT coalesce(gp_sql.schema_tags('tagged_s'::regnamespace)::text, 'none');" "none"
isl "ALTER USER ... TAG" \
   "ALTER USER tagged_u TAG (tier = 'gold');
    SELECT gp_sql.role_tags('tagged_u'::regrole)::text;" '{"env": "prod", "tier": "gold"}'
isl "and UNSET TAG" \
   "ALTER USER tagged_u UNSET TAG (env);
    SELECT gp_sql.role_tags('tagged_u'::regrole)::text;" '{"tier": "gold"}'
isl "ALTER SEQUENCE ... TAG, which is an action like any of ALTER SEQUENCE's" \
   "ALTER SEQUENCE tagged_seq TAG (tier = 'gold');
    SELECT gp_sql.relation_tags('tagged_seq'::regclass)::text;" '{"env": "prod", "tier": "gold"}'
isl "ALTER FOREIGN TABLE ... UNSET TAG" \
   "ALTER FOREIGN TABLE tagged_ft UNSET TAG (env);
    SELECT coalesce(gp_sql.relation_tags('tagged_ft'::regclass)::text, 'none');" "none"
q "ALTER DATABASE tagged_db TAG (env = 'dev');" > /dev/null
is "ALTER DATABASE ... TAG" \
   "SELECT gp_sql.database_tags('tagged_db')::text;" '{"env": "dev", "tier": "gold"}'
q "ALTER DATABASE tagged_db UNSET TAG (tier);" > /dev/null
is "and UNSET TAG" "SELECT gp_sql.database_tags('tagged_db')::text;" '{"env": "dev"}'
q "ALTER TABLESPACE tagged_ts2 TAG (tier = 'gold');" > /dev/null
is "ALTER TABLESPACE ... TAG, and its own options stay" \
   "SELECT gp_sql.tablespace_tags('tagged_ts2')::text || ' ' || array_to_string(spcoptions, ',')
      FROM pg_tablespace WHERE spcname = 'tagged_ts2';" '{"env": "prod", "tier": "gold"} random_page_cost=3.0'
q "ALTER TABLESPACE tagged_ts2 UNSET TAG (env);" > /dev/null
is "and UNSET TAG" "SELECT gp_sql.tablespace_tags('tagged_ts2')::text;" '{"tier": "gold"}'
# Cloudberry's tag test expects these to be syntax errors at TAG: its grammar
# has TAG on such an ALTER only on its own.  They used to run, as the ALTER
# followed by a call.  PostgreSQL's grammar would refuse them too, but a
# role's options may be any word, and it said "unrecognized role option";
# a database's may be any word with a value, and it stopped at the
# parenthesis.
at "a TAG beside another action of ALTER USER is Cloudberry's syntax error" \
   "ALTER USER tagged_u CONNECTION LIMIT 3 TAG (tier = 'silver')" 'syntax error at or near "TAG"' "TAG"
at "and of ALTER DATABASE" \
   "ALTER DATABASE tagged_db CONNECTION LIMIT 3 TAG (tier = 'silver')" 'syntax error at or near "TAG"' "TAG"
at "and of ALTER TABLESPACE" \
   "ALTER TABLESPACE tagged_ts2 SET (seq_page_cost = 1.1) TAG (tier = 'silver')" \
   'syntax error at or near "TAG"' "TAG"
at "and anything after one is, at what follows it" \
   "ALTER USER tagged_u TAG (tier = 'silver') CONNECTION LIMIT 3" \
   'syntax error at or near "CONNECTION"' "CONNECTION"
is "and nothing of any of them was done" \
   "SELECT rolconnlimit || ' ' || gp_sql.role_tags('tagged_u'::regrole)::text
      FROM pg_roles WHERE rolname = 'tagged_u';" '-1 {"tier": "gold"}'

###############################################################################
echo "4. DISTRIBUTED BY, which ORCA and M2's dispatch both read"
###############################################################################
isl "DISTRIBUTED BY (cols)" \
   "CREATE TABLE dist (a int, b int) DISTRIBUTED BY (a, b);
    SELECT gp_sql.distribution('dist'::regclass);" "(a,b)"
isl "DISTRIBUTED RANDOMLY" \
   "CREATE TABLE dist_r (a int) DISTRIBUTED RANDOMLY;
    SELECT gp_sql.distribution('dist_r'::regclass);" "random"
isl "DISTRIBUTED REPLICATED" \
   "CREATE TABLE dist_p (a int) DISTRIBUTED REPLICATED;
    SELECT gp_sql.distribution('dist_p'::regclass);" "replicated"
isl "and after the query of a CREATE TABLE AS" \
   "CREATE TABLE dist_as AS SELECT 1 AS a DISTRIBUTED BY (a);
    SELECT gp_sql.distribution('dist_as'::regclass) || ' ' || (SELECT a::text FROM dist_as);" \
   "(a) 1"
is "an ordinary table has no policy" \
   "SELECT gp_sql.distribution('plain'::regclass) IS NULL;" "t"
isl "both clauses on one statement" \
   "CREATE TABLE combo (a int) DISTRIBUTED BY (a) TAG (env = 'prod');
    SELECT gp_sql.distribution('combo'::regclass) || ' ' ||
           (gp_sql.relation_tags('combo'::regclass) ->> 'env');" "(a) prod"
isl "on CREATE FOREIGN TABLE, which has only OPTIONS to carry it in" \
   "CREATE FOREIGN TABLE dist_ft (a int, b int) SERVER tag_srv DISTRIBUTED BY (b);
    SELECT gp_sql.distribution('dist_ft'::regclass) || ' ' || coalesce(array_to_string(ftoptions, ','), 'none')
      FROM pg_foreign_table WHERE ftrelid = 'dist_ft'::regclass;" "(b) none"
# ALTER TABLE ... SET DISTRIBUTED BY moves a table's rows on a cluster
# (the cluster suite checks it); on one node, as in Cloudberry's single-node
# mode, it is refused in Cloudberry's words.  It is an option of the ALTER
# now, not a syntax error: it once became the ALTER, cut short, followed by a
# call, and then was left for PostgreSQL's grammar to refuse.
refused "ALTER TABLE ... SET DISTRIBUTED BY is refused on one node, as Cloudberry refuses it" \
        "ALTER TABLE combo SET DISTRIBUTED BY (a);" 'SET DISTRIBUTED BY not supported in utility mode'

# ONE STATEMENT FOR ONE.  These clauses used to become the statement followed
# by SELECTs of gp_sql's setters: two statements for one, which cannot be
# prepared -- a driver on the extended protocol, as JDBC and most are, got
# "cannot insert multiple commands into a prepared statement" -- and whose
# SELECT's row psql printed after CREATE TABLE.  The singlenode suite found
# it.  Now each is an option of the statement, in its WITH list, which
# gp_sql's hook takes out again; the statement stays one.
is "DISTRIBUTED BY and TAG become options of CREATE TABLE, and it stays one statement" \
   "SELECT gp_sql.desugar('CREATE TABLE one1 (a int) DISTRIBUTED BY (a) TAG (env = ''prod'')')
           ~ '^CREATE TABLE one1 \(a int\) +WITH \(gp_tag.env = ''prod'', gp.distributed_by = ''\(a\)''\) *$';" "t"
got=$(printf '%s\n' "CREATE TABLE ext1 (a int, b int) DISTRIBUTED BY (b) TAG (env = 'prod') \\bind \\g" \
                    "SELECT gp_sql.distribution('ext1'::regclass) || ' ' || (gp_sql.relation_tags('ext1'::regclass) ->> 'env');" |
      "$PSQL" -X -q -t -A -d postgres 2>&1)
[ "$got" = "(b) prod" ] && ok "so it can be prepared, as a driver on the extended protocol does" \
	|| notok "so it can be prepared, as a driver on the extended protocol does" "got [$got]"
isl "into the WITH list the statement has, if it has one" \
   "CREATE TABLE one2 (a int) WITH (fillfactor = 70) DISTRIBUTED BY (a);
    SELECT array_to_string(reloptions, ',') || ' ' || gp_sql.distribution('one2'::regclass)
      FROM pg_class WHERE relname = 'one2';" "fillfactor=70 (a)"
isl "and before AS, on CREATE TABLE AS" \
   "CREATE TABLE one4 AS SELECT 1 AS a, 2 AS b DISTRIBUTED BY (b) TAG (env = 'prod');
    SELECT gp_sql.distribution('one4'::regclass) || ' ' || (gp_sql.relation_tags('one4'::regclass) ->> 'env');" \
   "(b) prod"
# The tags used to go on the first relation the statement made, and a serial
# column's sequence is made before its table.
isl "a table with a serial column gets its tags, and its sequence none" \
   "CREATE TABLE one3 (id serial, a int) TAG (env = 'prod');
    SELECT gp_sql.relation_tags('one3'::regclass)::text || ' ' ||
           coalesce(gp_sql.relation_tags('one3_id_seq'::regclass)::text, 'none');" '{"env": "prod"} none'
isl "CREATE TABLE IF NOT EXISTS of one that is there leaves its tags alone" \
   "CREATE TABLE IF NOT EXISTS one3 (id int) TAG (env = 'dev');
    SELECT gp_sql.relation_tags('one3'::regclass) ->> 'env';" "prod"
isl "TAG on CREATE INDEX, which Cloudberry's grammar has too" \
   "CREATE INDEX one3_a ON one3 (a) TAG (env = 'prod');
    SELECT tagvalue FROM gp_sql.index_tag WHERE indexrelid = 'one3_a'::regclass;" "prod"
is "ALTER TABLE ... TAG is ALTER TABLE ... SET, in place" \
   "SELECT gp_sql.desugar('ALTER TABLE one3 ADD COLUMN c int, TAG (env = ''dev'')')
           ~ '^ALTER TABLE one3 ADD COLUMN c int, SET \(gp_tag.env = ''dev''\) *$';" "t"
isl "and runs as one" \
   "ALTER TABLE one3 ADD COLUMN c int, TAG (env = 'dev');
    SELECT (gp_sql.relation_tags('one3'::regclass) ->> 'env') || ' ' || count(*)
      FROM pg_attribute WHERE attrelid = 'one3'::regclass AND attname = 'c';" "dev 1"
isl "and UNSET TAG is RESET" \
   "ALTER TABLE one3 UNSET TAG (env);
    SELECT coalesce(gp_sql.relation_tags('one3'::regclass)::text, 'none');" "none"
is "where a statement's grammar has no place for them, they are carried on its parse node" \
   "SELECT gp_sql.desugar('CREATE SCHEMA s WITH TAG (env = ''prod'', tier = ''gold'')')
           ~ '^CREATE SCHEMA s +/\* and on its parse node: gp_tag.env = ''prod'', gp_tag.tier = ''gold'' \*/$';" "t"
is "and where PostgreSQL has no statement for them, they are a CALL" \
   "SELECT gp_sql.desugar('ALTER SCHEMA s TAG (env = ''prod'')') || ' | ' || gp_sql.desugar('DROP TAG t1, t2');" \
   "CALL gp_sql.alter_schema_tags('s'::regnamespace, set_tags => '{\"env\": \"prod\"}'::jsonb) | CALL gp_sql.drop_tag(ARRAY['t1', 't2']::name[], false)"
is "a foreign table's go into its OPTIONS, with its DISTRIBUTED BY" \
   "SELECT gp_sql.desugar('CREATE FOREIGN TABLE f (a int) SERVER s DISTRIBUTED BY (a) TAG (env = ''prod'')')
           ~ '^CREATE FOREIGN TABLE f \(a int\) SERVER s OPTIONS \(\"gp_tag.env\" ''prod'', \"gp.distributed_by\" ''\(a\)''\) *$';" "t"

# THE DEFECT THE PARENTHESES FIX, which was found while writing the reader
# that ORCA's relcache translator needs.  Written bare, a one-column list is
# the policy word: both of these recorded "random", and nothing downstream
# could tell a table hashed on a column called "random" from a randomly
# distributed one.  Two different distributions under one spelling, and ORCA
# would have been told the wrong one.
isl "a column called random is not DISTRIBUTED RANDOMLY" \
   "CREATE TABLE dist_word (random int, b int) DISTRIBUTED BY (random);
    SELECT gp_sql.distribution('dist_word'::regclass);" "(random)"
isl "and one called replicated is not DISTRIBUTED REPLICATED" \
   "CREATE TABLE dist_word2 (replicated int) DISTRIBUTED BY (replicated);
    SELECT gp_sql.distribution('dist_word2'::regclass);" "(replicated)"
is "so the two spellings no longer collide" \
   "SELECT gp_sql.distribution('dist_word'::regclass)
         <> gp_sql.distribution('dist_r'::regclass);" "t"

# A column name that needs quoting survives the round trip: the scanner has
# already downcased an unquoted name and dequoted a quoted one, so what goes
# into the label is the true column name, quoted again where it needs to be.
isl "a quoted column name keeps its case" \
   "CREATE TABLE dist_q (\"Mixed\" int) DISTRIBUTED BY (\"Mixed\");
    SELECT gp_sql.distribution('dist_q'::regclass);" '("Mixed")'
isl "and an unquoted one is downcased, as PostgreSQL downcases it" \
   "CREATE TABLE dist_u (Mixed int) DISTRIBUTED BY (Mixed);
    SELECT gp_sql.distribution('dist_u'::regclass);" "(mixed)"
isl "a column name holding a comma survives too" \
   "CREATE TABLE dist_c (\"a,b\" int) DISTRIBUTED BY (\"a,b\");
    SELECT gp_sql.distribution('dist_c'::regclass);" '("a,b")'

# The shape is refused when it is set, not ignored when it is read -- the
# rule the "gp" label already follows for its keys.  The old bare form is
# what a caller that has not been told about the parentheses would pass.
refused "the old bare column list is refused now" \
        "SELECT gp_sql.set_distribution('dist'::regclass, 'a,b');" \
        "unrecognized distribution policy \"a,b\""
refused "and so is anything else" \
        "SELECT gp_sql.set_distribution('dist'::regclass, 'sideways');" \
        "unrecognized distribution policy \"sideways\""
refused "and the error says what the accepted forms are" \
        "SELECT gp_sql.set_distribution('dist'::regclass, 'sideways');" \
        "parenthesised column list"

# And what the recorded text becomes when something reads it.  gp.policy() is
# gp_core's, not the rewriter's, but it is tested here because the round trip
# is the thing worth testing: the label is only as good as what comes back out
# of it, and the defect above was invisible until something tried to read one.
#
# ORCA's relcache translator asks this of every relation it sees, and turns
# the kind into EreldistrHash, EreldistrRandom, EreldistrReplicated or
# EreldistrMasterOnly.

is "a column list reads back as a hash policy" \
   "SELECT kind FROM gp.policy('dist'::regclass);" "hash"
is "with the key columns, in the order they were written" \
   "SELECT columns::text FROM gp.policy('dist'::regclass);" "{a,b}"
is "DISTRIBUTED RANDOMLY reads back as random, with no key" \
   "SELECT kind || ' ' || columns::text FROM gp.policy('dist_r'::regclass);" "random {}"
is "DISTRIBUTED REPLICATED reads back as replicated" \
   "SELECT kind FROM gp.policy('dist_p'::regclass);" "replicated"

# The same distinction the parentheses were added for, now from the reader's
# side: one is a key of one column, the other has no key at all.
is "a column called random is a hash policy on that column" \
   "SELECT kind || ' ' || columns::text FROM gp.policy('dist_word'::regclass);" \
   "hash {random}"

is "an unlabelled table has no policy, which is not an error" \
   "SELECT gp.policy('plain'::regclass) IS NULL;" "t"

# A NULL policy is what every table means on one node, so it is the common
# case here and not the odd one -- in Cloudberry only a catalog table has it.
is "and a catalog table has none either" \
   "SELECT gp.policy('pg_class'::regclass) IS NULL;" "t"

is "a quoted column name resolves to the column it names" \
   "SELECT columns::text FROM gp.policy('dist_q'::regclass);" '{Mixed}'
is "and so does one holding a comma, which is why it was quoted" \
   "SELECT columns::text FROM gp.policy('dist_c'::regclass);" '{"a,b"}'

# What ORCA carries into DXL beside the columns: the family each key column is
# hashed with.  PostgreSQL's default hash family for the type, which is what
# Cloudberry's cdb_default_distribution_opclass_for_type() also resolves to.
is "each key column reports its hash operator family" \
   "SELECT array_agg(f.opfname || '/' || am.amname ORDER BY f.oid)::text
      FROM gp.policy('dist'::regclass) p,
           unnest(p.opfamilies) AS u(oid)
      JOIN pg_opfamily f ON f.oid = u.oid
      JOIN pg_am am ON am.oid = f.opfmethod;" \
   "{integer_ops/hash,integer_ops/hash}"
is "which is the family PostgreSQL would hash that column with" \
   "SELECT opfamilies[1] = (SELECT oc.opcfamily FROM pg_opclass oc
                              JOIN pg_am am ON am.oid = oc.opcmethod
                             WHERE am.amname = 'hash'
                               AND oc.opcintype = 'int4'::regtype
                               AND oc.opcdefault)
      FROM gp.policy('dist'::regclass);" "t"
is "and a random policy has no families to report" \
   "SELECT opfamilies::text FROM gp.policy('dist_r'::regclass);" "{}"

# The segment count is not a flag: ORCA divides by it.  One, on one node.
is "the policy carries the segment count, which is never zero" \
   "SELECT numsegments >= 1 FROM gp.policy('dist_r'::regclass);" "t"

# A key column dropped leaves the table random, as Cloudberry leaves it: the
# label names the key's columns, and follows them (gp_sql's distribution.c).
q "CREATE TABLE dist_drop (a int, b int) DISTRIBUTED BY (a, b);
   ALTER TABLE dist_drop DROP COLUMN b;" > /dev/null
is "a key column dropped leaves the table random, as Cloudberry leaves it" \
   "SELECT kind FROM gp.policy('dist_drop'::regclass);" "random"

# A policy that cannot be read is an error naming the problem, not a shrug.
# Cloudberry cannot reach the first of these -- its policy is a catalog row --
# but a label written by hand can name a column the table has not got.
q "SECURITY LABEL FOR gp ON TABLE dist_drop IS 'distributed_by=\"(a,b)\"';" > /dev/null
refused "a key column the table has not got is named, not ignored" \
        "SELECT kind FROM gp.policy('dist_drop'::regclass);" \
        "column \"b\" of the distribution policy of \"dist_drop\" does not exist"

# What ORCA would otherwise do with an attribute number it cannot find is
# assert "Column not found", from inside the optimizer, with no column name
# in it.
q "CREATE TABLE dist_bad (a int);
   SECURITY LABEL FOR gp ON TABLE dist_bad IS 'distributed_by=sideways';" > /dev/null
refused "a shape the reader does not know is refused too" \
        "SELECT kind FROM gp.policy('dist_bad'::regclass);" \
        "unrecognized distribution policy \"sideways\""

q "SECURITY LABEL FOR gp ON TABLE dist_bad IS 'distributed_by=\"(a,)\"';" > /dev/null
refused "so is a column list with a hole in it" \
        "SELECT kind FROM gp.policy('dist_bad'::regclass);" \
        "empty column name"

# A label may be set by hand, so the reader cannot rely on the writer having
# checked the shape.
q "SECURITY LABEL FOR gp ON TABLE dist_bad IS 'distributed_by=\"(a\"';" > /dev/null
refused "and an unclosed one" \
        "SELECT kind FROM gp.policy('dist_bad'::regclass);" \
        "malformed distribution policy"

# A type with no default hash operator class cannot be a distribution key.
# Cloudberry refuses it when the table is created and gives this message; the
# port gives it when the policy is read, and says which column it was.
q "CREATE TABLE dist_nohash (p point);
   SECURITY LABEL FOR gp ON TABLE dist_nohash IS 'distributed_by=\"(p)\"';" > /dev/null
refused "a type that cannot be hashed cannot be a key" \
        "SELECT kind FROM gp.policy('dist_nohash'::regclass);" \
        "has no default operator class for access method \"hash\""
refused "and the error says which column it was" \
        "SELECT kind FROM gp.policy('dist_nohash'::regclass);" \
        "Column \"p\" of \"dist_nohash\" cannot be a distribution key."

###############################################################################
echo "5. incremental materialized views and dynamic tables"
###############################################################################
q "CREATE TABLE base (grp int, amt numeric); INSERT INTO base VALUES (1, 10);" > /dev/null
isl "CREATE INCREMENTAL MATERIALIZED VIEW" \
   "CREATE INCREMENTAL MATERIALIZED VIEW imv AS
      SELECT grp, count(*) AS n FROM base GROUP BY grp;
    INSERT INTO base VALUES (1, 5);
    SELECT n FROM imv WHERE grp = 1;" "2"
isl "CREATE DYNAMIC TABLE ... SCHEDULE" \
   "CREATE DYNAMIC TABLE dt SCHEDULE '0 3 * * *' AS SELECT count(*) AS n FROM base;
    SELECT gp_matview.dynamic_schedule('dt'::regclass);" "0 3 * * *"
isl "REFRESH DYNAMIC TABLE" \
   "INSERT INTO base VALUES (2, 1);
    REFRESH DYNAMIC TABLE dt;
    SELECT n FROM dt;" "3"
isl "DROP DYNAMIC TABLE" \
   "DROP DYNAMIC TABLE dt;
    SELECT count(*) FROM pg_class WHERE relname = 'dt';" "0"

###############################################################################
echo "6. directory tables and storage servers"
###############################################################################
isl "CREATE DIRECTORY TABLE" \
   "CREATE DIRECTORY TABLE docs;
    SELECT gp_sql.directory_table_location('docs'::regclass) IS NOT NULL;" "t"
is "which is the CREATE TABLE that makes one" \
   "SELECT gp_sql.desugar('CREATE DIRECTORY TABLE d TABLESPACE t')
           ~ '^CREATE TABLE d \(relative_path text PRIMARY KEY, size bigint, last_modified timestamptz, md5 text, tag text\) WITH \(gp.directory_table = true\) TABLESPACE t$';" "t"
isl "with its TAG, which it used to drop" \
   "CREATE DIRECTORY TABLE docs2 TAG (env = 'prod');
    SELECT gp_sql.relation_tags('docs2'::regclass)::text || ' ' ||
           (gp_sql.directory_table_location('docs2'::regclass) IS NOT NULL);" '{"env": "prod"} true'
isl "and IF NOT EXISTS of one that is there leaves it be" \
   "CREATE DIRECTORY TABLE IF NOT EXISTS docs2;
    SELECT count(*) FROM gp_sql.directory_tables WHERE tablename = 'docs2';" "1"
refused "WITH LOCATION, which it used to drop, is refused" \
        "CREATE DIRECTORY TABLE docs3 WITH LOCATION 'elsewhere';" \
        "WITH LOCATION is not supported for a directory table"
isl "CREATE STORAGE SERVER ... OPTIONS" \
   "CREATE STORAGE SERVER s3 OPTIONS (endpoint 's3.example.com');
    SELECT options::text FROM gp_sql.storage_servers WHERE servername = 's3';" \
   "{endpoint=s3.example.com}"
isl "ALTER STORAGE SERVER" \
   "ALTER STORAGE SERVER s3 OPTIONS (ADD region 'eu');
    SELECT options::text FROM gp_sql.storage_servers WHERE servername = 's3';" \
   "{endpoint=s3.example.com,region=eu}"
isl "CREATE STORAGE USER MAPPING" \
   "CREATE STORAGE USER MAPPING FOR CURRENT_USER STORAGE SERVER s3
      OPTIONS (secret 'shh');
    SELECT count(*) FROM gp_sql.storage_user_mappings WHERE servername = 's3';" "1"
isl "DROP STORAGE USER MAPPING and DROP STORAGE SERVER" \
   "DROP STORAGE USER MAPPING FOR CURRENT_USER STORAGE SERVER s3;
    DROP STORAGE SERVER s3;
    SELECT count(*) FROM gp_sql.storage_servers;" "0"

###############################################################################
echo "7. tasks"
###############################################################################
isl "CREATE TASK" \
   "CREATE TASK nightly SCHEDULE '0 2 * * *' AS 'VACUUM';
    SELECT schedule || ' | ' || command FROM gp_task.job WHERE jobname = 'nightly';" \
   "0 2 * * * | VACUUM"
isl "CREATE TASK ... DATABASE ... USER" \
   "CREATE TASK other SCHEDULE '* * * * *' DATABASE postgres USER postgres AS 'SELECT 1';
    SELECT database || '/' || username FROM gp_task.job WHERE jobname = 'other';" \
   "postgres/postgres"
isl "ALTER TASK" \
   "ALTER TASK nightly SCHEDULE '0 4 * * *';
    SELECT schedule FROM gp_task.job WHERE jobname = 'nightly';" "0 4 * * *"
# IF NOT EXISTS and IF EXISTS used to be read past, so each was an error where
# Cloudberry says what it did not do.
refused "CREATE TASK IF NOT EXISTS of one that is there says so and does nothing" \
        "CREATE TASK IF NOT EXISTS nightly SCHEDULE '0 5 * * *' AS 'VACUUM';" \
        'NOTICE:  task "nightly" already exists, skipping'
is "and it is as it was" "SELECT schedule FROM gp_task.job WHERE jobname = 'nightly';" "0 4 * * *"
refused "ALTER TASK IF EXISTS of one that is not says so" \
        "ALTER TASK IF EXISTS never_there SCHEDULE '0 1 * * *';" \
        'NOTICE:  task "never_there" does not exist, skipping'
isl "DROP TASK" \
   "DROP TASK nightly, other;
    SELECT count(*) FROM gp_task.job;" "0"

###############################################################################
echo "8. profiles and account locking"
###############################################################################
isl "CREATE PROFILE ... LIMIT" \
   "CREATE PROFILE strict LIMIT FAILED_LOGIN_ATTEMPTS 3 PASSWORD_LOCK_TIME 1;
    SELECT failed_login_attempts || '/' || password_lock_time
      FROM gp_security.profiles WHERE profile = 'strict';" "3/1"
isl "ALTER PROFILE ... LIMIT" \
   "ALTER PROFILE strict LIMIT PASSWORD_REUSE_MAX 5;
    SELECT password_reuse_max FROM gp_security.profiles WHERE profile = 'strict';" "5"
isl "ALTER USER ... PROFILE" \
   "CREATE USER carol LOGIN PASSWORD 'carol-pass';
    ALTER USER carol PROFILE strict;
    SELECT gp_security.role_profile('carol');" "strict"
isl "ALTER USER ... ACCOUNT LOCK" \
   "ALTER USER carol ACCOUNT LOCK;
    SELECT gp_security.role_locked_until('carol') = 'infinity'::timestamptz;" "t"
isl "ALTER USER ... ACCOUNT UNLOCK" \
   "ALTER USER carol ACCOUNT UNLOCK;
    SELECT gp_security.role_locked_until('carol') IS NULL;" "t"
refused "a profile that is not one is refused" \
        "ALTER USER carol PROFILE nope;" 'profile "nope" does not exist'
is "and carol is under the one she was" "SELECT gp_security.role_profile('carol');" "strict"
# NOPROFILE is one word, which no trigger word began, so on its own it was
# never rewritten; it worked only beside a DROP PROFILE in the same string.
is "ALTER USER ... NOPROFILE, on its own" \
   "ALTER USER carol NOPROFILE;" ""
is "takes the profile away" \
   "SELECT gp_security.role_profile('carol') IS NULL;" "t"
isl "DROP PROFILE" \
   "DROP PROFILE strict;
    SELECT count(*) FROM gp_security.profiles WHERE profile = 'strict';" "0"

###############################################################################
echo "9. what the rewrite does not disturb"
###############################################################################
is "an ALTER TABLE that only adds a column named tag" \
   "ALTER TABLE plain ADD COLUMN tag text;
    SELECT count(*) FROM pg_attribute
     WHERE attrelid = 'plain'::regclass AND attname = 'tag';" "1"
is "a numeric column after one called tag" \
   "CREATE TABLE tricky (tag numeric(10,2), distributed text);
    SELECT count(*) FROM pg_attribute
     WHERE attrelid = 'tricky'::regclass AND attnum > 0;" "2"
is "and PL/pgSQL, which parses through the same hook" \
   "CREATE FUNCTION plfn() RETURNS int LANGUAGE plpgsql AS \$\$
    DECLARE tag int := 7; BEGIN RETURN tag; END \$\$;
    SELECT plfn();" "7"
is "a prepared statement is parsed once and still works" \
   "PREPARE p AS SELECT \$1::int; EXECUTE p(42);" "42"

###############################################################################
echo "10. EXECUTE ON, which says where a function may run"
###############################################################################
# The label this writes is what func_exec_location() reads, and ORCA asks it
# of every function it meets.  The two halves are tested against the same
# strings: the ORCA suite asserts the reading, this one the writing.
#
# The clause is SET gp.execute_on = '...' in the statement it is on, which
# gp_sql takes out again and writes as the label once the function exists:
# one statement, where it used to be the statement and a SECURITY LABEL.
#
# Cloudberry allows EXECUTE ON anything but ANY only on a function that
# returns a set (validate_sql_exec_location), so the functions here do.

label_of() { echo "SELECT label FROM pg_seclabel WHERE objoid = '$1'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';"; }

isl "EXECUTE ON ALL SEGMENTS" \
   "CREATE FUNCTION xf1(int) RETURNS SETOF int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON ALL SEGMENTS;
    $(label_of 'xf1(int)')" \
   "execute_on=all_segments"

isl "EXECUTE ON ANY, which any function may say" \
   "CREATE FUNCTION xf2(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON ANY;
    $(label_of 'xf2(int)')" \
   "execute_on=any"

isl "EXECUTE ON COORDINATOR" \
   "CREATE FUNCTION xf3(int) RETURNS SETOF int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON COORDINATOR;
    $(label_of 'xf3(int)')" \
   "execute_on=coordinator"

# Cloudberry's older spelling, which its own grammar still accepts and maps to
# the same value.  A label a person reads says "coordinator" either way.
isl "EXECUTE ON MASTER, which means the same thing" \
   "CREATE FUNCTION xf4(int) RETURNS SETOF int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON MASTER;
    $(label_of 'xf4(int)')" \
   "execute_on=coordinator"

isl "EXECUTE ON INITPLAN" \
   "CREATE FUNCTION xf5(int) RETURNS SETOF int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON INITPLAN;
    $(label_of 'xf5(int)')" \
   "execute_on=initplan"

isl "ALTER FUNCTION changes it" \
   "ALTER FUNCTION xf1(int) EXECUTE ON COORDINATOR;
    $(label_of 'xf1(int)')" \
   "execute_on=coordinator"

isl "a parameter with a DEFAULT" \
   "CREATE FUNCTION xf6(a int, b int DEFAULT 5) RETURNS SETOF int LANGUAGE sql
      AS 'SELECT a + b' EXECUTE ON ALL SEGMENTS;
    $(label_of 'xf6(int,int)')" \
   "execute_on=all_segments"

isl "an OUT parameter" \
   "CREATE FUNCTION xf7(IN a int, OUT b int) RETURNS SETOF int LANGUAGE sql AS 'SELECT a'
      EXECUTE ON ALL SEGMENTS;
    $(label_of 'xf7(int)')" \
   "execute_on=all_segments"

isl "a type that is spelled with a comma inside parentheses" \
   "CREATE FUNCTION xf8(a numeric(10,2)) RETURNS SETOF int LANGUAGE sql AS 'SELECT 1'
      EXECUTE ON ALL SEGMENTS;
    $(label_of 'xf8(numeric)')" \
   "execute_on=all_segments"

isl "a schema-qualified name" \
   "CREATE SCHEMA xs;
    CREATE FUNCTION xs.xf9(int) RETURNS SETOF int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON ALL SEGMENTS;
    $(label_of 'xs.xf9(int)')" \
   "execute_on=all_segments"

isl "a procedure, which shares the clause in Cloudberry's grammar" \
   "CREATE PROCEDURE xp1(int) LANGUAGE sql AS 'SELECT \$1' EXECUTE ON ANY;
    $(label_of 'xp1(int)')" \
   "execute_on=any"

is "a function written without the clause carries no label" \
   "CREATE FUNCTION xf10(int) RETURNS int LANGUAGE sql AS 'SELECT \$1';
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'xf10(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass;" "0"

# Cloudberry's rule, and its message.  The port used to take the clause on any
# function.
refused "EXECUTE ON ALL SEGMENTS on a function that returns one row is refused" \
        "CREATE FUNCTION xbad1(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
           EXECUTE ON ALL SEGMENTS;" \
        "EXECUTE ON ALL SEGMENTS is only supported for set-returning functions"
refused "and so is EXECUTE ON COORDINATOR" \
        "CREATE FUNCTION xbad2(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
           EXECUTE ON COORDINATOR;" \
        "EXECUTE ON COORDINATOR is only supported for set-returning functions"
refused "and by ALTER as well" \
        "ALTER FUNCTION xf2(int) EXECUTE ON INITPLAN;" \
        "EXECUTE ON INITPLAN is only supported for set-returning functions"
is "the refused functions were not made" \
   "SELECT count(*) FROM pg_proc WHERE proname LIKE 'xbad%';" "0"
is "and the refused ALTER changed nothing" \
   "$(label_of 'xf2(int)')" "execute_on=any"

# THE COLLISION WORTH TESTING.  "EXECUTE ON" is also how every GRANT of the
# execute privilege is written, and the rewriter now looks for the word
# "execute" in every statement.  What keeps them apart is that the clause is
# only read on a CREATE or ALTER of a function.
is "GRANT EXECUTE ON FUNCTION is not a place to run one" \
   "GRANT EXECUTE ON FUNCTION xf10(int) TO PUBLIC;
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'xf10(int)'::regprocedure;" "0"

is "and REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA is not either" \
   "REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA public FROM PUBLIC;
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'xf10(int)'::regprocedure;" "0"

is "the words inside a function body are left where they are" \
   "CREATE FUNCTION xf11() RETURNS text LANGUAGE sql
      AS \$\$ SELECT 'EXECUTE ON ALL SEGMENTS' \$\$;
    SELECT xf11();" "EXECUTE ON ALL SEGMENTS"

is "the clause is an option of the statement, in its place" \
   "SELECT gp_sql.desugar('CREATE FUNCTION g(int) RETURNS SETOF int LANGUAGE sql
      AS ''SELECT 1'' EXECUTE ON ALL SEGMENTS')
         = 'CREATE FUNCTION g(int) RETURNS SETOF int LANGUAGE sql
      AS ''SELECT 1'' SET gp.execute_on = ''all_segments''';" "t"

is "a statement with no clause is handed back untouched" \
   "SELECT gp_sql.desugar('GRANT EXECUTE ON FUNCTION xf10(int) TO PUBLIC')
         = 'GRANT EXECUTE ON FUNCTION xf10(int) TO PUBLIC';" "t"

# The option is not stored as a setting of the function, which would send
# every call through the security-definer path and keep it from being inlined.
is "and it is not left as a setting of the function" \
   "SELECT count(*) FROM pg_proc WHERE proconfig IS NOT NULL AND proname LIKE 'xf%';" "0"

# ALTER FUNCTION f(int) EXECUTE ON ANY is a whole statement of Cloudberry's:
# with the option taken out, ALTER FUNCTION has nothing left to do and does
# nothing, as it would -- but checks the function is the user's.  Beside
# another action the ALTER does the rest.
isl "ALTER FUNCTION with another action keeps the ALTER" \
   "ALTER FUNCTION xf1(int) STRICT EXECUTE ON ALL SEGMENTS;
    SELECT proisstrict::text || ' ' ||
           (SELECT label FROM pg_seclabel WHERE objoid = p.oid
              AND classoid = 'pg_proc'::regclass AND provider = 'gp')
      FROM pg_proc p WHERE p.oid = 'xf1(int)'::regprocedure;" \
   "true execute_on=all_segments"

# The form Track F gives for writing the label by hand is the same option.
isl "SET gp.execute_on written by hand is the same thing" \
   "ALTER FUNCTION xf3(int) SET gp.execute_on = 'initplan';
    $(label_of 'xf3(int)')" "execute_on=initplan"
isl "and RESET takes it away" \
   "ALTER FUNCTION xf3(int) RESET gp.execute_on;
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'xf3(int)'::regprocedure;" "0"
refused "a value it does not have is refused" \
        "ALTER FUNCTION xf3(int) SET gp.execute_on = 'segments';" \
        'invalid value for gp.execute_on: "segments"'

###############################################################################
echo
echo "11. the data-access attributes, and what they compose with"
###############################################################################
# NO SQL, CONTAINS SQL, READS SQL DATA, MODIFIES SQL DATA.  Nothing reads
# them, in the port or in Cloudberry: prodataaccess is written by pg_proc.c,
# defaulted and validated in functioncmds.c, and read nowhere outside the DDL
# path.  So the whole feature is the syntax, the three rules below, and a
# place to keep the answer.

isl "NO SQL on a plpgsql function" \
   "CREATE FUNCTION da1() RETURNS int LANGUAGE plpgsql NO SQL
      AS \$\$ BEGIN RETURN 1; END \$\$;
    $(label_of 'da1()')" \
   "data_access=none"

isl "CONTAINS SQL" \
   "CREATE FUNCTION da2(int) RETURNS int LANGUAGE sql CONTAINS SQL
      AS 'SELECT \$1';
    $(label_of 'da2(int)')" \
   "data_access=contains"

isl "READS SQL DATA" \
   "CREATE FUNCTION da3(int) RETURNS int LANGUAGE sql READS SQL DATA
      AS 'SELECT \$1';
    $(label_of 'da3(int)')" \
   "data_access=reads"

isl "MODIFIES SQL DATA" \
   "CREATE FUNCTION da4(int) RETURNS int LANGUAGE sql MODIFIES SQL DATA
      AS 'SELECT \$1';
    $(label_of 'da4(int)')" \
   "data_access=modifies"

# A procedure takes them too.
isl "a procedure takes them as well" \
   "CREATE PROCEDURE da5(int) LANGUAGE sql READS SQL DATA AS 'SELECT \$1';
    $(label_of 'da5(int)')" \
   "data_access=reads"

# --- Cloudberry's three rules ------------------------------------------------
#
# They are validate_sql_data_access() in its functioncmds.c, and they are the
# only thing the attribute does.  They are checked against the function as the
# statement leaves it, before the statement is over, so a function that breaks
# one is not left behind.
refused "IMMUTABLE conflicts with READS SQL DATA" \
   "CREATE FUNCTION dbad1(int) RETURNS int LANGUAGE sql IMMUTABLE
      READS SQL DATA AS 'SELECT \$1';" \
   "IMMUTABLE conflicts with READS SQL DATA."

refused "IMMUTABLE conflicts with MODIFIES SQL DATA" \
   "CREATE FUNCTION dbad2(int) RETURNS int LANGUAGE sql IMMUTABLE
      MODIFIES SQL DATA AS 'SELECT \$1';" \
   "IMMUTABLE conflicts with MODIFIES SQL DATA."

refused "a SQL function cannot say NO SQL" \
   "CREATE FUNCTION dbad3(int) RETURNS int LANGUAGE sql NO SQL
      AS 'SELECT \$1';" \
   "A SQL function cannot specify NO SQL."

is "and the rejected function was not created" \
   "SELECT count(*) FROM pg_proc WHERE proname LIKE 'dbad%';" "0"

isl "IMMUTABLE with CONTAINS SQL is allowed" \
   "CREATE FUNCTION da6(int) RETURNS int LANGUAGE sql IMMUTABLE CONTAINS SQL
      AS 'SELECT \$1';
    $(label_of 'da6(int)')" \
   "data_access=contains"

# Cloudberry holds every ALTER FUNCTION to them, whatever it changed, so a
# function that reads can no longer be made IMMUTABLE.
refused "ALTER FUNCTION ... IMMUTABLE of a function that READS SQL DATA is refused" \
   "ALTER FUNCTION da3(int) IMMUTABLE;" "IMMUTABLE conflicts with READS SQL DATA."
is "and the function is as it was" \
   "SELECT provolatile FROM pg_proc WHERE oid = 'da3(int)'::regprocedure;" "v"

# --- composing with EXECUTE ON -----------------------------------------------
isl "both clauses on one function write both keys" \
   "CREATE FUNCTION da7(int) RETURNS SETOF int LANGUAGE sql READS SQL DATA
      EXECUTE ON ALL SEGMENTS AS 'SELECT \$1';
    $(label_of 'da7(int)')" \
   "execute_on=all_segments,data_access=reads"

# The keys come out in a fixed order, so two spellings of the same function
# produce the same label rather than two that compare unequal.
isl "written the other way round, the label is the same" \
   "CREATE FUNCTION da8(int) RETURNS SETOF int LANGUAGE sql EXECUTE ON ALL SEGMENTS
      READS SQL DATA AS 'SELECT \$1';
    $(label_of 'da8(int)')" \
   "execute_on=all_segments,data_access=reads"

is "and the two agree" \
   "SELECT (SELECT label FROM pg_seclabel WHERE objoid = 'da7(int)'::regprocedure
              AND classoid = 'pg_proc'::regclass AND provider = 'gp')
         = (SELECT label FROM pg_seclabel WHERE objoid = 'da8(int)'::regprocedure
              AND classoid = 'pg_proc'::regclass AND provider = 'gp');" "t"

# THE DEFECT ONE LABEL PER STATEMENT HAD.  The clause of an ALTER was a
# SECURITY LABEL, which replaces the provider's whole label, so setting one
# key took the other away.
isl "ALTER of one key keeps the other" \
   "ALTER FUNCTION da7(int) EXECUTE ON ANY;
    $(label_of 'da7(int)')" \
   "execute_on=any,data_access=reads"
isl "in the same order, whichever it changed" \
   "ALTER FUNCTION da8(int) CONTAINS SQL;
    $(label_of 'da8(int)')" \
   "execute_on=all_segments,data_access=contains"

# --- ALTER -------------------------------------------------------------------
isl "ALTER FUNCTION can set it on its own" \
   "ALTER FUNCTION da2(int) MODIFIES SQL DATA;
    $(label_of 'da2(int)')" \
   "data_access=modifies"

isl "and beside another action the ALTER still does the rest" \
   "ALTER FUNCTION da3(int) STRICT CONTAINS SQL;
    SELECT proisstrict::text || ' ' ||
           (SELECT label FROM pg_seclabel WHERE objoid = p.oid
              AND classoid = 'pg_proc'::regclass AND provider = 'gp')
      FROM pg_proc p WHERE p.oid = 'da3(int)'::regprocedure;" \
   "true data_access=contains"

# --- CREATE OR REPLACE says what both are ------------------------------------
#
# Cloudberry's ProcedureCreate writes both columns whenever a function is
# made or replaced, so one the statement does not name goes back to its
# default -- which the label spells by leaving the key out.
isl "CREATE OR REPLACE with one clause takes the other away" \
   "CREATE OR REPLACE FUNCTION da7(int) RETURNS SETOF int LANGUAGE sql
      EXECUTE ON ALL SEGMENTS AS 'SELECT \$1';
    $(label_of 'da7(int)')" \
   "execute_on=all_segments"
is "and with none, both" \
   "CREATE OR REPLACE FUNCTION da7(int) RETURNS SETOF int LANGUAGE sql AS 'SELECT \$1';
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'da7(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass;" "0"

# --- what the trigger pair buys, and what is observable of it ----------------
#
# The four forms all end in SQL, so the trigger is the pair rather than either
# word: "sql" alone would fire on every LANGUAGE sql, which is most function
# DDL, and "no" on a large share of ordinary SQL.
#
# The prefilter itself is invisible from here on purpose -- a statement it
# skips and a statement the rewriter finds nothing in are both handed back
# unchanged -- so what these check is the contract rather than the
# optimization: ordinary SQL comes back byte for byte, and NO SQL does not.
is "an ordinary statement with 'no' in it comes back unchanged" \
   "SELECT gp_sql.desugar('SELECT 1 WHERE no_such_column IS NULL')
         = 'SELECT 1 WHERE no_such_column IS NULL';" "t"

is "and one with 'sql' in it does too" \
   "SELECT gp_sql.desugar('SELECT ''sql'' AS sql') = 'SELECT ''sql'' AS sql';" "t"

is "a function with only LANGUAGE sql is not rewritten" \
   "SELECT gp_sql.desugar('CREATE FUNCTION z(int) RETURNS int LANGUAGE sql AS ''x''')
         = 'CREATE FUNCTION z(int) RETURNS int LANGUAGE sql AS ''x''';" "t"

is "but NO SQL is seen" \
   "SELECT gp_sql.desugar('CREATE FUNCTION z() RETURNS int LANGUAGE plpgsql NO SQL AS ''x''') LIKE '%SET gp.data_access = ''none''%';" "t"

# "node_sql" is one word, not "no" followed by "sql", and must not be rewritten.
is "a word that merely starts with a trigger is left alone" \
   "SELECT gp_sql.desugar('SELECT node_sql FROM t') = 'SELECT node_sql FROM t';" "t"

# --- the default is absence --------------------------------------------------
#
# Cloudberry fills in CONTAINS SQL for a LANGUAGE SQL function and NO SQL for
# everything else at DDL time.  Here the absence of the key means exactly
# that, so an ordinary function carries no label at all.
is "a function with no clause carries no label" \
   "CREATE FUNCTION da9(int) RETURNS int LANGUAGE sql AS 'SELECT \$1';
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'da9(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" "0"

echo
echo "12. median(), DECODE and CASE x WHEN IS NOT DISTINCT FROM y"

# --- median(), gp_core's -------------------------------------------------------
#
# A plain aggregate, not the ordered-set one Cloudberry's grammar makes of
# MEDIAN(x), and the answer percentile_cont(0.5)'s: the middle row, or halfway
# between the middle two.  Cloudberry has it for float8, interval, timestamp
# and timestamptz, and so does the port, in pg_catalog where Cloudberry has it.

is "median() is four aggregates in pg_catalog, one per type Cloudberry has" \
   "SELECT string_agg(a, ', ' ORDER BY a)
      FROM (SELECT pg_get_function_arguments(oid) AS a FROM pg_proc
             WHERE proname = 'median' AND prokind = 'a'
               AND pronamespace = 'pg_catalog'::regnamespace) p;" \
   "double precision, interval, timestamp with time zone, timestamp without time zone"

is "the middle row of an odd count" \
   "SELECT median(x) FROM (VALUES (3.0::float8), (1), (2)) v(x);" "2"

is "halfway between the middle two of an even one" \
   "SELECT median(x) FROM (VALUES (4.0::float8), (1), (3), (2)) v(x);" "2.5"

# bfv_aggregate's own case: median() of an integer column is its float8.
is "an integer column, which is a float8 to median()" \
   "SELECT median(i), pg_typeof(median(i)) FROM generate_series(1, 100) i;" \
   "50.5|double precision"

is "null rows are not counted, and none at all is NULL" \
   "SELECT median(x), (SELECT median(y) FROM (VALUES (NULL::float8)) n(y)) IS NULL,
           (SELECT median(z) FROM generate_series(1, 0) z) IS NULL
      FROM (VALUES (1.0::float8), (NULL), (3)) v(x);" "2|t|t"

is "percentile_cont(0.5)'s answer, over float8" \
   "SELECT median(x) = percentile_cont(0.5) WITHIN GROUP (ORDER BY x)
      FROM (SELECT (i * 7919 % 1000) / 7.0 AS x FROM generate_series(1, 1000) i) s;" "t"

is "and over interval" \
   "SELECT median(x) = percentile_cont(0.5) WITHIN GROUP (ORDER BY x)
      FROM (SELECT (i * 7919 % 1000) * interval '1 minute' AS x
              FROM generate_series(1, 1000) i) s;" "t"

is "timestamp, halfway" \
   "SELECT median(x) FROM (VALUES (timestamp '2020-01-02'), (timestamp '2020-01-01')) v(x);" \
   "2020-01-01 12:00:00"

# Cloudberry's timestamp_lerp() rounds the scaled difference, as round() does.
is "and a half microsecond rounded away from zero, as Cloudberry rounds it" \
   "SELECT median(x) FROM (VALUES (timestamp '2020-01-01 00:00:00'),
                                  (timestamp '2020-01-01 00:00:00.000001')) v(x);" \
   "2020-01-01 00:00:00.000001"

is "timestamptz" \
   "SET TimeZone = 'UTC';
    SELECT median(x) FROM (VALUES (timestamptz '2020-01-01 00:00+00'),
                                  (timestamptz '2020-01-01 01:00+00'),
                                  (timestamptz '2020-01-03 00:00+00')) v(x);" \
   "2020-01-01 01:00:00+00"

is "halfway to an infinity is that infinity" \
   "SELECT median(x) FROM (VALUES (timestamp '2020-01-01'), (timestamp 'infinity')) v(x);" \
   "infinity"

refused "and between the two there is no answer" \
        "SELECT median(x) FROM (VALUES (timestamp '-infinity'), (timestamp 'infinity')) v(x);" \
        "timestamp out of range"

is "grouped" \
   "SELECT string_agg(g || '=' || m, ' ' ORDER BY g) FROM
      (SELECT i % 3 AS g, median(i) AS m FROM generate_series(1, 10) i GROUP BY 1) s;" \
   "0=6 1=5.5 2=5"

# Two median(x) in one query share one transition state, and the final
# function runs on it twice: the sort is read twice.
is "two calls over the same rows share a state and both answer" \
   "SELECT median(i), median(i) + 1 FROM generate_series(1, 9) i;" "5|6"

is "past work_mem the rows go to disk, as percentile_cont's do" \
   "SET work_mem = '64kB';
    SELECT median(i) FROM generate_series(1, 200000) i;" "100000.5"

# The planner may hash median()'s groups, each with a sort of its own, and
# spill them to disk past work_mem as it spills any hashed group; ORCA sorts
# them, having no combine function to merge a spilled group with.
is "hashed or sorted, grouped past work_mem, the answers are percentile_cont's" \
   "CREATE TABLE med_g AS SELECT i % 100 AS k, (i * 7919) % 100003 AS v
      FROM generate_series(1, 20000) i;
    ANALYZE med_g;
    CREATE VIEW med_pct AS SELECT md5(string_agg(k || ':' || m, ',' ORDER BY k)) AS h FROM
      (SELECT k, percentile_cont(0.5) WITHIN GROUP (ORDER BY v) AS m FROM med_g GROUP BY k) s;
    CREATE VIEW med_med AS SELECT md5(string_agg(k || ':' || m, ',' ORDER BY k)) AS h FROM
      (SELECT k, median(v) AS m FROM med_g GROUP BY k) s;
    SET work_mem = '64kB';
    SET enable_sort = off;
    SELECT (SELECT h FROM med_med) = (SELECT h FROM med_pct);
    RESET enable_sort;
    SET enable_hashagg = off;
    SELECT (SELECT h FROM med_med) = (SELECT h FROM med_pct);" "t
t"

# Over a window it is computed by its moving-aggregate implementation: a
# window calls the final function for every row and adds the next rows to the
# same state, which a sorted tuplesort cannot take.  Cloudberry's grammar has
# no OVER after MEDIAN (...), so there each of these is a syntax error.
is "a running median, over a frame that only grows" \
   "SELECT string_agg(m::text, ' ' ORDER BY i) FROM
      (SELECT i, median(i) OVER (ORDER BY i) AS m FROM generate_series(1, 6) i) s;" \
   "1 1.5 2 2.5 3 3.5"

is "over each partition whole" \
   "SELECT string_agg(DISTINCT g || '=' || m, ' ' ORDER BY g || '=' || m) FROM
      (SELECT i % 3 AS g, median(i) OVER (PARTITION BY i % 3) AS m
         FROM generate_series(1, 10) i) s;" "0=6 1=5.5 2=5"

q "CREATE TABLE medw AS
     SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE ((i * 7919) % 23) / 2.0 END::float8 AS x,
            interval '1 minute' * ((i * 31) % 17) AS iv,
            timestamp '2020-01-01' + interval '1 hour' * ((i * 13) % 29) AS ts
       FROM generate_series(1, 300) i;" > /dev/null

# A frame that moves takes rows out as well as in, through the inverse
# function; each frame is checked against percentile_cont over the same rows,
# with the duplicates and nulls medw has.
is "over a sliding frame, as percentile_cont over the same rows" \
   "SELECT count(*) FROM
      (SELECT i, median(x) OVER (ORDER BY i ROWS BETWEEN 6 PRECEDING AND 3 FOLLOWING) AS m
         FROM medw) a
     WHERE m IS DISTINCT FROM (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x)
                                 FROM medw b WHERE b.i BETWEEN a.i - 6 AND a.i + 3);" "0"

is "and over interval and timestamp, by RANGE" \
   "SELECT count(*) FROM
      (SELECT i, median(iv) OVER w AS mi, median(ts) OVER w AS mt FROM medw
        WINDOW w AS (ORDER BY i RANGE BETWEEN 4 PRECEDING AND CURRENT ROW)) a
     WHERE mi IS DISTINCT FROM (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY iv)
                                  FROM medw b WHERE b.i BETWEEN a.i - 4 AND a.i)
        OR mt IS DISTINCT FROM (SELECT median(ts) FROM medw b WHERE b.i BETWEEN a.i - 4 AND a.i);" "0"

is "with the current row excluded, which the window recomputes for each row" \
   "SELECT count(*) FROM
      (SELECT i, median(x) OVER (ORDER BY i ROWS BETWEEN 3 PRECEDING AND 3 FOLLOWING
                                  EXCLUDE CURRENT ROW) AS m
         FROM medw) a
     WHERE m IS DISTINCT FROM (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x)
                                 FROM medw b WHERE b.i BETWEEN a.i - 3 AND a.i + 3
                                  AND b.i <> a.i);" "0"

is "a window over all the rows answers as the aggregate does" \
   "SELECT (SELECT DISTINCT median(x) OVER () FROM medw) = (SELECT median(x) FROM medw);" "t"

# A volatile argument is evaluated again as its row leaves the frame, and is
# then another value than the one that went in; the inverse function sees it
# is not the oldest value's and says so, and the window starts the frame again.
is "a volatile argument, whose value changes as its row leaves, restarts the frame" \
   "SELECT count(m), bool_and(m >= 0 AND m < 1) FROM
      (SELECT median(random()) OVER (ORDER BY i ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS m
         FROM generate_series(1, 50) i) s;" "50|t"

# Cloudberry prints MEDIAN(a) with the cast hidden; PostgreSQL 19 shows an
# aggregate argument's implicit cast, as it does for any aggregate.
isl "a view prints it as it was written, and an integer's cast to float8" \
   "CREATE TABLE med (a int, f float8);
    CREATE VIEW med_v AS SELECT median(f) AS m, median(a) AS n FROM med;
    SELECT regexp_replace(pg_get_viewdef('med_v'::regclass, true), '\\s+', ' ', 'g');" \
   " SELECT median(f) AS m, median(a::double precision) AS n FROM med;"

# --- DECODE --------------------------------------------------------------------
#
# Cloudberry's parser makes DECODE(x, a, r, ...) a CASE that compares x with IS
# NOT DISTINCT FROM; PostgreSQL 19 compares a CASE's operand with = and nothing
# else, so the rewrite is a searched CASE with x in each arm.

is "DECODE is a searched CASE, IS NOT DISTINCT FROM in each arm" \
   "SELECT gp_sql.desugar('SELECT decode(a, 1, ''one'', 2, ''two'', ''other'') FROM t');" \
   "SELECT CASE WHEN (a) IS NOT DISTINCT FROM (1) THEN 'one' WHEN (a) IS NOT DISTINCT FROM (2) THEN 'two' ELSE 'other' END FROM t"

is "with an even count of arguments, the last is its default" \
   "SELECT decode(2, 1, 'ABC', 2, 'DEF'), decode(3, 1, 'ABC', 'none'), decode(3, 1, 'ABC') IS NULL;" \
   "DEF|none|t"

is "and NULL matches NULL, which = never does" \
   "SELECT decode(null, null, true, false), decode(NULL::int, 1, 100, NULL, 200, 300);" "t|200"

is "PostgreSQL's two-argument decode() is left to be PostgreSQL's" \
   "SELECT gp_sql.desugar('SELECT decode(''aGk='', ''base64'')') = 'SELECT decode(''aGk='', ''base64'')',
           convert_from(decode('aGk=', 'base64'), 'UTF8');" "t|hi"

# case_gp's own: DECODE with three arguments is the CASE even where a function
# called decode takes three, and a quoted or qualified name calls the function.
is "unquoted it is DECODE whatever functions exist; quoted or qualified, the function" \
   "CREATE FUNCTION \"decode\"(int, int, int) RETURNS int LANGUAGE sql IMMUTABLE
      AS 'SELECT \$1 * \$2 - \$3';
    SELECT decode(11, 8, 11) IS NULL, \"decode\"(11, 8, 11), public.decode(11, 8, 11);
    DROP FUNCTION \"decode\"(int, int, int);" "t|77|77"

# What PostgreSQL 19 lets decode be that Cloudberry, where it is reserved, did not.
is "a function may be created with the name, unquoted" \
   "CREATE FUNCTION decode(a int, b int, c int) RETURNS int LANGUAGE sql AS 'SELECT 0';
    DROP FUNCTION decode(int, int, int);
    SELECT 'made';" "made"

is "and a table, with its column list, inserted into by name" \
   "CREATE TABLE decode (a int, b int, c int);
    INSERT INTO decode (a, b, c) VALUES (decode(1, 1, 7), 2, 3);
    SELECT a FROM decode;" "7"

is "and a query's name, with its columns" \
   "WITH decode (x, y, z) AS (SELECT 1, 2, 3) SELECT x + y + z FROM decode;" "6"

is "and an alias, with its columns" \
   "SELECT decode.z FROM (VALUES (1, 2, 3)) decode (x, y, z);" "3"

is "none of which is rewritten" \
   "SELECT gp_sql.desugar('CREATE INDEX ON decode (a, b, c)') = 'CREATE INDEX ON decode (a, b, c)'
       AND gp_sql.desugar('DROP FUNCTION f(int), decode(int, int, int)')
         = 'DROP FUNCTION f(int), decode(int, int, int)';" "t"

is "and a call wherever an expression may begin" \
   "SELECT DISTINCT ON (x) decode(x, 1, 'one', 'other') FROM (VALUES (1), (1), (2)) v(x) ORDER BY x LIMIT 1;
    CREATE TABLE decode_using (c int);
    ALTER TABLE decode_using ALTER COLUMN c TYPE text USING decode(c, 1, 'one');
    SELECT position('e' IN decode(1, 1, 'one')), overlay('xxx' PLACING decode(1, 1, 'y') FROM 2);" \
   "one
3|xyx"

is "DECODE inside DECODE" \
   "SELECT decode(decode(1, 1, 2), 2, 'nested', 'not');" "nested"

is "grouped by, where both spellings are one expression" \
   "SELECT string_agg(k || ':' || n, ' ' ORDER BY k) FROM
      (SELECT decode(a % 2, 0, 'even', 'odd') AS k, count(*) AS n FROM generate_series(1, 5) a
        GROUP BY decode(a % 2, 0, 'even', 'odd')) s;" "even:2 odd:3"

isl "its column is called case, as Cloudberry's is" \
   "CREATE TABLE decode_col AS SELECT decode(1, 1, 'x');
    SELECT attname FROM pg_attribute WHERE attrelid = 'decode_col'::regclass AND attnum > 0;" \
   "case"

# The one difference that shows: Cloudberry's CASE evaluates its operand once,
# and a searched CASE evaluates x in each arm it tries.  Pinned so that it is
# known, not fixed: no grammar can make PostgreSQL 19 compare a CASE's operand
# with anything but =.
isl "the first argument is evaluated in each arm tried, not once" \
   "CREATE SEQUENCE decode_seq;
    SELECT decode(nextval('decode_seq'), 100, 'x', 200, 'y', 'z');
    SELECT currval('decode_seq');" "2"

# --- CASE x WHEN IS NOT DISTINCT FROM y ------------------------------------------

is "the CASE form, and its other arms compared with =" \
   "SELECT gp_sql.desugar('SELECT CASE a WHEN IS NOT DISTINCT FROM 1 THEN ''x'' WHEN 2 THEN ''y'' ELSE ''z'' END FROM t');" \
   "SELECT CASE WHEN (a) IS NOT DISTINCT FROM (1) THEN 'x' WHEN (a) = (2) THEN 'y' ELSE 'z' END FROM t"

is "a CASE without it is PostgreSQL's, and untouched" \
   "SELECT gp_sql.desugar('SELECT CASE a WHEN 1 THEN 2 END, decode(a, 1) FROM t')
         = 'SELECT CASE a WHEN 1 THEN 2 END, decode(a, 1) FROM t';" "t"

is "it answers as Cloudberry's does" \
   "SELECT string_agg(coalesce(g, '-') || '=' || CASE g
               WHEN IS NOT DISTINCT FROM 'M' THEN 'Male'
               WHEN IS NOT DISTINCT FROM null THEN 'Not Specified'
               WHEN 'F' THEN 'Female'
               ELSE 'Other' END, ' ' ORDER BY n)
      FROM (VALUES (1, 'F'), (2, 'M'), (3, NULL), (4, 'Z')) v(n, g);" \
   "F=Female M=Male -=Not Specified Z=Other"

is "nested, in each other and in DECODE" \
   "SELECT CASE decode(1, 1, 1) WHEN IS NOT DISTINCT FROM 1 THEN
               CASE NULL::int WHEN IS NOT DISTINCT FROM NULL THEN 'both' END END;" "both"

# --- PL/pgSQL, whose expressions come to the parser alone ------------------------
#
# The CASE in the IF is in parentheses because PL/pgSQL ends an IF's condition
# at the first THEN it meets outside a bracket, a CASE's own included; that is
# PostgreSQL's rule for any CASE there, not this rewrite's.

is "in a PL/pgSQL expression, an assignment and a condition" \
   "CREATE FUNCTION pl_decode(x int) RETURNS text LANGUAGE plpgsql AS \$\$
    DECLARE v text;
    BEGIN
      v := decode(x, 1, 'one', 'other');
      IF (CASE x WHEN IS NOT DISTINCT FROM NULL THEN true ELSE false END) THEN
        RETURN 'null';
      END IF;
      RETURN v || '/' || decode(x, 2, 'two', 'not two');
    END \$\$;
    SELECT pl_decode(1) || ' ' || pl_decode(2) || ' ' || pl_decode(NULL);" \
   "one/not two other/two null"

is "and in the body of a SQL function, standard or quoted" \
   "CREATE FUNCTION sql_decode1(x int) RETURNS text LANGUAGE sql RETURN decode(x, 1, 'one', 'other');
    CREATE FUNCTION sql_decode2(x int) RETURNS text LANGUAGE sql AS 'SELECT decode(x, 1, ''one'', ''other'')';
    SELECT sql_decode1(1) || ' ' || sql_decode2(2);" "one other"

echo
echo "13. where a rewritten statement's errors are reported, and what is recorded of it"

# A rewrite is longer than what it replaces, and PostgreSQL reports positions in
# the text its grammar read.  Each error below is reported where the user wrote
# what it is about: an error after a rewrite, in what the rewrite copied, and in
# what it wrote, which stands for the token Cloudberry's grammar would have put
# the error at.

at "an error after a DECODE is where the user wrote it" \
   "SELECT decode(1, 1, 'x'), nosuchcol FROM generate_series(1, 2)" \
   'column "nosuchcol" does not exist' "nosuchcol"

at "and so is a syntax error after one" \
   "SELECT decode(1, 1, 'x') FROM @@FROM" 'syntax error at or near "FROM"' "FROM"

at "an error inside one, in what it copied" \
   "SELECT decode(1, 1, ARRAY[1], 0)" "CASE types integer and integer[] cannot be matched" "ARRAY"

at "IS NOT DISTINCT FROM with no operator, at the value compared, as Cloudberry's" \
   "SELECT decode('a'::text, 1, 'x')" "operator does not exist: text = integer" "1,"

at "the CASE form's, at the arm's NOT" \
   "SELECT CASE 'a'::text WHEN IS NOT DISTINCT FROM 1 THEN 'x' END" \
   "operator does not exist: text = integer" "NOT"

at "and an ordinary arm's =, at its WHEN" \
   "SELECT CASE current_date WHEN IS NOT DISTINCT FROM current_date THEN 1 @@WHEN 2007 THEN 2 END" \
   "operator does not exist: date = integer" "WHEN"

# The map is the rewrite's, not DECODE's: a clause that moved into a WITH list
# moves nothing that comes after it any more.
at "and after a clause of Cloudberry's that the rewrite moved" \
   "CREATE TABLE pos_t (a int) DISTRIBUTED BY (a) nonsense" 'syntax error at or near "nonsense"' "nonsense"

# pg_stat_statements cuts each statement of a string out of the user's text by
# the start and length its parse tree carries, and replaces its constants by
# theirs.  Those were the rewrite's: at 364a988298a the SELECT here was
# recorded as "er_it", and a rewrite longer than what it replaced put a
# statement past the end of the text, which an assert-enabled server stops on
# in CleanQuerytext.  The constants the rewrite writes -- the 'any' of the SET
# that EXECUTE ON ANY becomes -- are in no text of the user's, and are left
# alone: charged to the clause, they stopped such a server too, and one left
# where the rewrite put it would be cut out of the user's text past the end of
# its statement.  So would the value of a SET in a statement after one the
# rewrite changed, which is at the rewrite's positions like everything else.
#
# Each statement is recorded once.  CREATE SCHEMA ... WITH TAG was two, the
# schema and the call that tagged it, and was recorded twice.
q "CREATE EXTENSION pg_stat_statements;
   CREATE FUNCTION pss_f(int) RETURNS int LANGUAGE sql AS 'SELECT 1';
   CREATE ROLE pss_r;" > /dev/null
q "SELECT pg_stat_statements_reset();" > /dev/null
q "ALTER FUNCTION pss_f(int) EXECUTE ON ANY; SELECT decode(7, 7, 'seven', 8, 'eight') AS d, 42 AS after_it;
   CREATE SCHEMA pss_s WITH TAG (env = 'prod'); ALTER ROLE pss_r SET work_mem = '2MB';" > /dev/null
is "pg_stat_statements has each statement's own text, its constants where they were written" \
   "SELECT string_agg(query, ' | ' ORDER BY query) FROM pg_stat_statements
     WHERE query LIKE '%pss_%' OR query LIKE '%after_it%';" \
   "ALTER FUNCTION pss_f(int) EXECUTE ON ANY | ALTER ROLE pss_r SET work_mem = \$1 | CREATE SCHEMA pss_s WITH TAG (env = 'prod') | SELECT decode(\$1, \$2, \$3, \$4, \$5) AS d, \$6 AS after_it"

# Cloudberry's grammar refuses these where the arm stops being IS NOT DISTINCT
# FROM, and so does this, at the same word.
at "without an operand there is no CASE form, and NOT is the error" \
   "SELECT CASE WHEN IS NOT DISTINCT FROM 1 THEN 2 END" 'syntax error at or near "NOT"' "NOT"

at "IS DISTINCT FROM is not the form" \
   "SELECT CASE 1 WHEN IS NOT DISTINCT FROM 2 THEN 'x' WHEN IS @@DISTINCT FROM 2 THEN 'y' END" \
   'syntax error at or near "DISTINCT"' "DISTINCT"

at "nor is IS NOT DISTINCT without FROM" \
   "SELECT CASE 'a' WHEN IS NOT DISTINCT 'b' THEN 'x' END" "syntax error at or near \"'b'\"" "'b'"

echo
echo "14. one statement for one, answering as the statement does"

# Every statement of Cloudberry's is one of PostgreSQL's once rewritten, so a
# driver on the extended protocol -- JDBC, and most others -- can prepare it,
# and psql's \bind sends it as they do.  TAG on CREATE SCHEMA, CREATE USER,
# CREATE SEQUENCE and CREATE FOREIGN TABLE, a foreign table's DISTRIBUTED BY,
# and CREATE FUNCTION's EXECUTE ON and data-access attributes were each the
# statement and a SELECT or a SECURITY LABEL after it, which the extended
# protocol refuses.
#
# And each answers as a statement does, with its command tag and no row.  One
# PostgreSQL has no counterpart of is a CALL, whose tag is CALL: CREATE TAG,
# CREATE PROFILE and the rest were a SELECT of a function, which answered with
# a row of nothing and SELECT 1.

# session <name> <psql input> <what psql prints>: not quiet, so a statement
# that answers with a command tag prints it.  \parse answers with an empty
# one, and its blank line is left out.
session() {
	local got; got=$(printf '%s\n' "$2" | "$PSQL" -X -t -A -d postgres 2>&1 | grep -v '^$')
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

# answers <name> <one statement, without its semicolon> <its command tag>
answers() { session "$1" "$2 \\bind \\g" "$3"; }

session "what \\bind sends is prepared, and two statements cannot be" \
        'SELECT 1\; SELECT 2 \bind \g' \
        "ERROR:  cannot insert multiple commands into a prepared statement"

answers "CREATE TAG" "CREATE TAG st_env ALLOWED_VALUES 'prod', 'dev'" "CALL"
answers "ALTER TAG" "ALTER TAG st_env ADD ALLOWED_VALUES 'test'" "CALL"
answers "DROP TAG" "CREATE TAG st_gone \\bind \\g
DROP TAG st_gone" "CALL
CALL"
is "and each did what it says" \
   "SELECT string_agg(tagname || ' ' || array_to_string(allowed_values, ','), ' ')
      FROM gp_sql.tag WHERE tagname LIKE 'st\_%';" "st_env prod,dev,test"

answers "TAG on CREATE SCHEMA" "CREATE SCHEMA st_s WITH TAG (st_env = 'prod')" "CREATE SCHEMA"
answers "on CREATE USER" "CREATE USER st_u TAG (st_env = 'prod')" "CREATE ROLE"
answers "on CREATE SEQUENCE" "CREATE SEQUENCE st_q TAG (st_env = 'prod')" "CREATE SEQUENCE"
answers "on CREATE FOREIGN TABLE" \
        "CREATE FOREIGN TABLE st_ft (a int) SERVER tag_srv TAG (st_env = 'prod')" "CREATE FOREIGN TABLE"
answers "DISTRIBUTED BY on a foreign table" \
        "CREATE FOREIGN TABLE st_ft2 (a int) SERVER tag_srv DISTRIBUTED BY (a)" "CREATE FOREIGN TABLE"
is "and the tags and the policy are where they were put" \
   "SELECT concat_ws(' ', gp_sql.schema_tags('st_s'::regnamespace) ->> 'st_env',
                          gp_sql.role_tags('st_u'::regrole) ->> 'st_env',
                          gp_sql.relation_tags('st_q'::regclass) ->> 'st_env',
                          gp_sql.relation_tags('st_ft'::regclass) ->> 'st_env',
                          gp_sql.distribution('st_ft2'::regclass));" "prod prod prod prod (a)"

answers "CREATE FUNCTION ... EXECUTE ON, with a data-access attribute" \
        "CREATE FUNCTION st_f(int) RETURNS SETOF int LANGUAGE sql READS SQL DATA
           EXECUTE ON ALL SEGMENTS AS 'SELECT \$1'" "CREATE FUNCTION"
answers "ALTER FUNCTION ... EXECUTE ON, on its own" \
        "ALTER FUNCTION st_f(int) EXECUTE ON COORDINATOR" "ALTER FUNCTION"
is "and the function's label says both" "$(label_of 'st_f(int)')" \
   "execute_on=coordinator,data_access=reads"

answers "CREATE PROFILE" "CREATE PROFILE st_p LIMIT FAILED_LOGIN_ATTEMPTS 3" "CALL"
answers "ALTER PROFILE" "ALTER PROFILE st_p LIMIT PASSWORD_REUSE_MAX 2" "CALL"
answers "ALTER USER ... PROFILE, which is ALTER ROLE" "ALTER USER st_u PROFILE st_p" "ALTER ROLE"
is "and the profile is the user's" "SELECT gp_security.role_profile('st_u');" "st_p"
answers "ALTER USER ... ACCOUNT LOCK" "ALTER USER st_u ACCOUNT LOCK" "ALTER ROLE"
is "and the account is locked" \
   "SELECT gp_security.role_locked_until('st_u') = 'infinity'::timestamptz;" "t"
answers "ALTER USER ... NOPROFILE" "ALTER USER st_u NOPROFILE" "ALTER ROLE"
answers "DROP PROFILE" "DROP PROFILE st_p" "CALL"
is "and the profile is gone, from the user and the catalog" \
   "SELECT (gp_security.role_profile('st_u') IS NULL) || ' ' ||
           (SELECT count(*) FROM gp_security.profiles WHERE profile = 'st_p');" "true 0"

answers "ALTER USER ... TAG" "ALTER USER st_u TAG (st_env = 'dev')" "ALTER ROLE"
answers "ALTER SCHEMA ... TAG, which is a CALL" "ALTER SCHEMA st_s TAG (st_env = 'dev')" "CALL"
answers "ALTER DATABASE ... TAG" "ALTER DATABASE tagged_db TAG (st_env = 'dev')" "ALTER DATABASE"
answers "ALTER TABLESPACE ... TAG" "ALTER TABLESPACE tagged_ts TAG (st_env = 'dev')" "ALTER TABLESPACE"
answers "ALTER SEQUENCE ... TAG" "ALTER SEQUENCE st_q TAG (st_env = 'dev')" "ALTER SEQUENCE"
answers "ALTER FOREIGN TABLE ... UNSET TAG" \
        "ALTER FOREIGN TABLE st_ft UNSET TAG (st_env)" "ALTER FOREIGN TABLE"
is "and each changed its tags" \
   "SELECT concat_ws(' ', gp_sql.role_tags('st_u'::regrole) ->> 'st_env',
                          gp_sql.schema_tags('st_s'::regnamespace) ->> 'st_env',
                          gp_sql.database_tags('tagged_db') ->> 'st_env',
                          gp_sql.tablespace_tags('tagged_ts') ->> 'st_env',
                          gp_sql.relation_tags('st_q'::regclass) ->> 'st_env',
                          coalesce(gp_sql.relation_tags('st_ft'::regclass)::text, 'none'));" \
   "dev dev dev dev dev none"

answers "CREATE TASK" "CREATE TASK st_t SCHEDULE '0 2 * * *' AS 'VACUUM'" "CALL"
answers "ALTER TASK" "ALTER TASK st_t SCHEDULE '0 3 * * *'" "CALL"
is "and the task is as it was altered" \
   "SELECT schedule FROM gp_task.job WHERE jobname = 'st_t';" "0 3 * * *"
answers "DROP TASK" "DROP TASK st_t" "CALL"

answers "CREATE DIRECTORY TABLE, which is CREATE TABLE" "CREATE DIRECTORY TABLE st_docs" "CREATE TABLE"
is "and it is one" \
   "SELECT gp_sql.directory_table_location('st_docs'::regclass) IS NOT NULL;" "t"

# A prepared statement is parsed once and run as often as it is executed, and
# what the rewrite put in its parse tree is taken out of a copy, never of the
# tree the plan cache keeps: run again, the statement does it again.  Between
# the two runs the object is put back as it was.
session "a prepared statement that carries tags does it each time it runs" \
        "ALTER USER st_u TAG (st_env = 'prod') \\parse st_tag_u
\\bind_named st_tag_u \\g
ALTER USER st_u UNSET TAG (st_env);
\\bind_named st_tag_u \\g
SELECT gp_sql.role_tags('st_u'::regrole) ->> 'st_env';" \
        "ALTER ROLE
ALTER ROLE
ALTER ROLE
prod"
session "and so does one with EXECUTE ON" \
        "ALTER FUNCTION st_f(int) EXECUTE ON ALL SEGMENTS \\parse st_eo
\\bind_named st_eo \\g
ALTER FUNCTION st_f(int) EXECUTE ON INITPLAN;
\\bind_named st_eo \\g
$(label_of 'st_f(int)')" \
        "ALTER FUNCTION
ALTER FUNCTION
ALTER FUNCTION
execute_on=all_segments,data_access=reads"

###############################################################################
echo
echo "15. Cloudberry's classic partition clauses"
###############################################################################

# PARTITION BY ... (START (...) END (...) EVERY (...), DEFAULT PARTITION ...)
# on CREATE TABLE, and ADD, DROP, ALTER, EXCHANGE, RENAME, SPLIT and TRUNCATE
# PARTITION and SET SUBPARTITION TEMPLATE on ALTER TABLE.  The rewrite checks
# the clause with a parser of Cloudberry's productions (gp_partition.c) and
# carries it, as written, in one option of the statement; gp_sql makes the
# partitions once the table exists.  What is checked here: what the rewrite
# hands the grammar, the syntax errors Cloudberry's tests expect at the token
# they expect them, the partitions made -- names, bounds, subpartitions,
# owner, privileges, distribution -- and what each ALTER TABLE command does.

# --- what the rewrite hands PostgreSQL's grammar -------------------------------

is "PostgreSQL's own PARTITION BY, where PostgreSQL has it, is left alone" \
   "SELECT gp_sql.desugar('CREATE TABLE t (a int) PARTITION BY RANGE (a)');" \
   "CREATE TABLE t (a int) PARTITION BY RANGE (a)"
is "and a window's, though it has PARTITION and it is in no bracket of the rewrite's" \
   "SELECT gp_sql.desugar('SELECT rank() OVER (PARTITION BY a) FROM t');" \
   "SELECT rank() OVER (PARTITION BY a) FROM t"
is "the classic clause: the key stays PostgreSQL's, the rest rides in the WITH list as written" \
   "SELECT gp_sql.desugar('CREATE TABLE t (a int, b int) PARTITION BY RANGE (b) (START (1) END (3) EVERY (1))');" \
   'CREATE TABLE t (a int, b int) PARTITION BY RANGE (b) WITH (gp.partition_by = $gp$PARTITION BY RANGE (b) (START (1) END (3) EVERY (1))$gp$)'
is "after DISTRIBUTED BY, the key moves to where PostgreSQL has it" \
   "SELECT gp_sql.desugar('CREATE TABLE t (a int, b int) DISTRIBUTED BY (a) PARTITION BY LIST (b) (PARTITION p VALUES (1), DEFAULT PARTITION other)');" \
   'CREATE TABLE t (a int, b int)  PARTITION BY LIST (b)    WITH (gp.distributed_by = '"'"'(a)'"'"', gp.partition_by = $gp$PARTITION BY LIST (b) (PARTITION p VALUES (1), DEFAULT PARTITION other)$gp$)'
is "and so does PostgreSQL's own, written at the end as Cloudberry lets it be" \
   "SELECT gp_sql.desugar('CREATE TABLE t (a int) WITH (fillfactor = 70) PARTITION BY RANGE (a)');" \
   "CREATE TABLE t (a int)  PARTITION BY RANGE (a) WITH (fillfactor = 70)  "
is "the option goes before TABLESPACE, where PostgreSQL has WITH" \
   "SELECT gp_sql.desugar('CREATE TABLE t (a int, b int) PARTITION BY RANGE (b) (START (1) END (2)) TABLESPACE pg_default');" \
   'CREATE TABLE t (a int, b int) PARTITION BY RANGE (b) WITH (gp.partition_by = $gp$PARTITION BY RANGE (b) (START (1) END (2))$gp$) TABLESPACE pg_default'
is "a dollar quote in the clause, and the option's is another" \
   "SELECT gp_sql.desugar(\$q\$CREATE TABLE t (a text) PARTITION BY LIST (a) (PARTITION p VALUES ('\$gp\$'))\$q\$);" \
   "CREATE TABLE t (a text) PARTITION BY LIST (a) WITH (gp.partition_by = \$gp1\$PARTITION BY LIST (a) (PARTITION p VALUES ('\$gp\$'))\$gp1\$)"
is "each ALTER TABLE command becomes a SET among the statement's commands" \
   "SELECT gp_sql.desugar('ALTER TABLE t ADD PARTITION p3 START (3) END (4), DROP PARTITION p1');" \
   'ALTER TABLE t SET (gp.partition_cmd = $gp$ADD PARTITION p3 START (3) END (4)$gp$), SET (gp.partition_cmd = $gp$DROP PARTITION p1$gp$)'

# ADD, DROP, ALTER and RENAME are PostgreSQL's too, about a column called
# "partition", which PostgreSQL 19 lets be a name and Cloudberry reserves.
# Where PostgreSQL's grammar takes one, it is PostgreSQL's.
for c in "ALTER TABLE t DROP partition" "ALTER TABLE t ADD partition int" \
         "ALTER TABLE t RENAME partition TO part" "ALTER TABLE t ALTER partition TYPE bigint"; do
	is "PostgreSQL's reading stands: $c" "SELECT gp_sql.desugar('$c');" "$c"
done
q "CREATE TABLE gpp_col (a int, partition int);" > /dev/null
is "and does what it does in PostgreSQL" \
   "ALTER TABLE gpp_col RENAME partition TO part; ALTER TABLE gpp_col DROP part;
    ALTER TABLE gpp_col ADD partition int;
    SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
     WHERE attrelid = 'gpp_col'::regclass AND attnum > 0 AND NOT attisdropped;" "a,partition"

# --- syntax errors, where Cloudberry's grammar raises them ---------------------
#
# Each of these is one of Cloudberry's tests' (partition, partition1,
# bfv_partition, partition_ddl), raised when the statement is parsed.

at "SUBPARTITION TEMPLATE without SUBPARTITION BY" \
   "CREATE TABLE gpp_e (a int, b text) PARTITION BY LIST (b) SUBPARTITION TEMPLATE (SUBPARTITION usa VALUES ('usa'))" \
   'syntax error at or near "TEMPLATE"' "TEMPLATE"
at "EVERY takes its value in parentheses" \
   "CREATE TABLE gpp_e (a int, b int) PARTITION BY RANGE (b) (START (1) END (20) EVERY 5 (1))" \
   'syntax error at or near "5"' "5"
at "a default partition has no boundary" \
   "CREATE TABLE gpp_e (a int, b int) PARTITION BY RANGE (b) (DEFAULT PARTITION x START (0) END (2))" \
   'syntax error at or near "START"' "START"
at "a strategy is a word" \
   "CREATE TABLE gpp_e (a int, b int) PARTITION BY (b) (START (1) END (10))" \
   'syntax error at or near "("' "(b"
refused "an unknown one is Cloudberry's error, which has no position" \
        "CREATE TABLE gpp_e (a int, b int) PARTITION BY funky (b) (START (1) END (10))" \
        'unrecognized partitioning strategy "funky"'
at "an expression is no key of the classic syntax" \
   "CREATE TABLE gpp_e (a int, b int) PARTITION BY RANGE ((b + 1)) (START (1) END (10))" \
   "expressions in partition key not supported in legacy GPDB partition syntax" "(b + 1)"
at "a syntax error in a boundary's expression is PostgreSQL's grammar's, where it is" \
   "CREATE TABLE gpp_e (a int, b int) PARTITION BY RANGE (b) (START (1 + ) END (10))" \
   'syntax error at or near ")"' ") END"
at "a template holds partitions, and NULL is none" \
   "ALTER TABLE gpp_e SET SUBPARTITION TEMPLATE (NULL)" 'syntax error at or near "NULL"' "NULL"
at "FOR takes a value" \
   "ALTER TABLE gpp_e DROP PARTITION FOR ()" 'syntax error at or near ")"' ")"
at "a call is no value: Cloudberry's grammar stops after it" \
   "ALTER TABLE gpp_e DROP PARTITION FOR (funky(1)@@)" 'syntax error at or near ")"' ")"
refused "and RANK(n) is Greenplum 6's, refused by name" \
        "ALTER TABLE gpp_e DROP PARTITION FOR (RANK(1))" \
        "addressing partition by RANK is no longer supported"
at "ADD PARTITION has no EVERY" \
   "ALTER TABLE gpp_e ADD PARTITION START (3) END (4) EVERY (1)" 'syntax error at or near "EVERY"' "EVERY"
at "DEFAULT is no partition's name" \
   "ALTER TABLE gpp_e ADD DEFAULT PARTITION @@default" 'syntax error at or near "default"' "default"
refused "a default partition is added by name" \
        "ALTER TABLE gpp_e ADD DEFAULT PARTITION FOR (1)" "can only ADD a partition by name"
refused "CREATE TABLE AS takes no partition clause, as in Cloudberry" \
        "CREATE TABLE gpp_e AS SELECT 1 AS a PARTITION BY LIST (a) (PARTITION p VALUES (1))" \
        "cannot create a partitioned table using CREATE TABLE AS SELECT"
at "nor may a CREATE TABLE have two" \
   "CREATE TABLE gpp_e (a int) PARTITION BY RANGE (a) (START (1) END (2)) DISTRIBUTED BY (a) @@PARTITION BY RANGE (a) (START (1) END (2))" \
   "only one PARTITION BY clause is allowed" "PARTITION"
refused "a template's partition has no partitions of its own" \
        "CREATE TABLE gpp_e (a int, b int) PARTITION BY RANGE (a) SUBPARTITION BY RANGE (b)
           SUBPARTITION TEMPLATE (SUBPARTITION s START (1) END (2) (SUBPARTITION x START (1) END (2)))
           (START (1) END (2))" \
        "template cannot contain specification for child partition"

# --- the partitions made --------------------------------------------------------

# parts <table>: its partitions and their bounds, a level at a time, as the
# partitions of each are ordered by name.
parts() {
	q "SELECT string_agg(c.relname || ' ' || pg_get_expr(c.relpartbound, c.oid), '; '
	                    ORDER BY t.level, c.relname)
	     FROM pg_partition_tree('$1') t JOIN pg_class c ON c.oid = t.relid WHERE t.level > 0;"
}
isparts() {
	local got; got=$(parts "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

q "CREATE TABLE gpp_r (a int, b date) DISTRIBUTED BY (a) PARTITION BY RANGE (b)
     (START (date '2020-01-01') INCLUSIVE END (date '2020-04-01') EXCLUSIVE EVERY (interval '1 month'),
      DEFAULT PARTITION other);" > /dev/null
isparts "EVERY steps with the key's +, and the default partition is numbered first" gpp_r \
   "gpp_r_1_prt_2 FOR VALUES FROM ('2020-01-01') TO ('2020-02-01'); gpp_r_1_prt_3 FOR VALUES FROM ('2020-02-01') TO ('2020-03-01'); gpp_r_1_prt_4 FOR VALUES FROM ('2020-03-01') TO ('2020-04-01'); gpp_r_1_prt_other DEFAULT"
is "each partition is distributed as the table is" \
   "SELECT count(*) FROM pg_seclabel s JOIN pg_class c ON c.oid = s.objoid
     WHERE s.provider = 'gp' AND s.label = 'distributed_by=(a)' AND c.relname LIKE 'gpp_r%';" "5"

q "CREATE TABLE gpp_k (i int) PARTITION BY RANGE (i) (START (0) EXCLUSIVE END (100) INCLUSIVE EVERY (25));" > /dev/null
isparts "START EXCLUSIVE and END INCLUSIVE are PostgreSQL's bounds, a step in" gpp_k \
   "gpp_k_1_prt_1 FOR VALUES FROM (1) TO (26); gpp_k_1_prt_2 FOR VALUES FROM (26) TO (51); gpp_k_1_prt_3 FOR VALUES FROM (51) TO (76); gpp_k_1_prt_4 FOR VALUES FROM (76) TO (101)"
q "CREATE TABLE gpp_m (i bigint) PARTITION BY RANGE (i) (PARTITION hi START (9223372036854775806) END (9223372036854775807) INCLUSIVE);" > /dev/null
isparts "and an END INCLUSIVE at the type's largest value is MAXVALUE" gpp_m \
   "gpp_m_1_prt_hi FOR VALUES FROM ('9223372036854775806') TO (MAXVALUE)"

q "CREATE TABLE gpp_i (a int) PARTITION BY RANGE (a)
     (PARTITION a START (0), PARTITION b START (3), PARTITION c START (5) END (8), PARTITION d END (20));" > /dev/null
isparts "a START or END left out is the neighbour's, or MINVALUE and MAXVALUE at the ends" gpp_i \
   "gpp_i_1_prt_a FOR VALUES FROM (0) TO (3); gpp_i_1_prt_b FOR VALUES FROM (3) TO (5); gpp_i_1_prt_c FOR VALUES FROM (5) TO (8); gpp_i_1_prt_d FOR VALUES FROM (8) TO (20)"

q "CREATE TABLE gpp_s (id int, region text, d date) PARTITION BY LIST (region)
     SUBPARTITION BY RANGE (d) SUBPARTITION TEMPLATE (START (date '2020-01-01') END (date '2020-03-01') EVERY (interval '1 month'))
     (PARTITION usa VALUES ('usa'), PARTITION asia VALUES ('asia', 'japan'), DEFAULT PARTITION rest);" > /dev/null
isparts "SUBPARTITION BY and its template, a level at a time" gpp_s \
   "gpp_s_1_prt_asia FOR VALUES IN ('asia', 'japan'); gpp_s_1_prt_rest DEFAULT; gpp_s_1_prt_usa FOR VALUES IN ('usa'); gpp_s_1_prt_asia_2_prt_1 FOR VALUES FROM ('2020-01-01') TO ('2020-02-01'); gpp_s_1_prt_asia_2_prt_2 FOR VALUES FROM ('2020-02-01') TO ('2020-03-01'); gpp_s_1_prt_rest_2_prt_1 FOR VALUES FROM ('2020-01-01') TO ('2020-02-01'); gpp_s_1_prt_rest_2_prt_2 FOR VALUES FROM ('2020-02-01') TO ('2020-03-01'); gpp_s_1_prt_usa_2_prt_1 FOR VALUES FROM ('2020-01-01') TO ('2020-02-01'); gpp_s_1_prt_usa_2_prt_2 FOR VALUES FROM ('2020-02-01') TO ('2020-03-01')"
is "the template is kept with the table, as written, in its gp label" \
   "SELECT label FROM pg_seclabel WHERE objoid = 'gpp_s'::regclass AND provider = 'gp';" \
   "partition_templates=1:78:(START (date '2020-01-01') END (date '2020-03-01') EVERY (interval '1 month'))"
is "rows go where the bounds say" \
   "INSERT INTO gpp_s VALUES (1, 'japan', '2020-02-15'), (2, 'usa', '2020-01-02'), (3, 'mars', '2020-01-31');
    SELECT string_agg(tableoid::regclass || ':' || id, ' ' ORDER BY id) FROM gpp_s;" \
   "gpp_s_1_prt_asia_2_prt_2:1 gpp_s_1_prt_usa_2_prt_1:2 gpp_s_1_prt_rest_2_prt_1:3"
at "an error in a bound value is reported at the value" \
   "CREATE TABLE gpp_e (a int) PARTITION BY LIST (a) (PARTITION p VALUES (1, 'abc'))" \
   'invalid input syntax for type integer: "abc"' "'abc'"
refused "a WITH (appendonly = false) partition is heap, which it names" \
        "CREATE TABLE gpp_ao (a int, b int) PARTITION BY LIST (b)
           (PARTITION p VALUES (1) WITH (appendonly = false, fillfactor = 70), PARTITION q VALUES (2) WITH (appendonly = true));" \
        'access method "ao_row" does not exist'
refused "and gp.max_partition_level limits the levels, as gp_max_partition_level does" \
        "SET gp.max_partition_level = 1;
         CREATE TABLE gpp_e (a int, b int) PARTITION BY RANGE (a) SUBPARTITION BY RANGE (b)
           SUBPARTITION TEMPLATE (START (1) END (2)) (START (1) END (2));" \
        "Exceeds maximum configured partitioning level of 1"

# --- ALTER TABLE ----------------------------------------------------------------

q "CREATE ROLE gpp_owner; GRANT CREATE ON SCHEMA public TO gpp_owner;
   SET ROLE gpp_owner;
   CREATE TABLE gpp_a (a int, b int) PARTITION BY RANGE (b) (PARTITION p1 START (0) END (10), PARTITION p2 START (10) END (20));
   RESET ROLE;
   CREATE ROLE gpp_reader; GRANT SELECT ON gpp_a TO gpp_reader;" > /dev/null
q "ALTER TABLE gpp_a ADD PARTITION p3 START (20) END (30);" > /dev/null
isparts "ADD PARTITION" gpp_a \
   "gpp_a_1_prt_p1 FOR VALUES FROM (0) TO (10); gpp_a_1_prt_p2 FOR VALUES FROM (10) TO (20); gpp_a_1_prt_p3 FOR VALUES FROM (20) TO (30)"
is "a partition a superuser adds is the table owner's, as in Cloudberry" \
   "SELECT relowner::regrole FROM pg_class WHERE relname = 'gpp_a_1_prt_p3';" "gpp_owner"
is "GRANT on the table reached its partitions, and the new one has the table's privileges" \
   "SELECT string_agg(c.relname || '=' || has_table_privilege('gpp_reader', c.oid, 'SELECT'), ' ' ORDER BY c.relname)
      FROM pg_class c WHERE c.relname LIKE 'gpp_a%';" \
   "gpp_a=true gpp_a_1_prt_p1=true gpp_a_1_prt_p2=true gpp_a_1_prt_p3=true"
is "as does one made with PostgreSQL's syntax" \
   "CREATE TABLE gpp_a_pg PARTITION OF gpp_a FOR VALUES FROM (40) TO (50);
    SELECT has_table_privilege('gpp_reader', 'gpp_a_pg', 'SELECT');" "t"
q "ALTER TABLE gpp_a ADD PARTITION p4 END (35);" > /dev/null
isparts "an ADD with no START starts where the partition below it ends" gpp_a \
   "gpp_a_1_prt_p1 FOR VALUES FROM (0) TO (10); gpp_a_1_prt_p2 FOR VALUES FROM (10) TO (20); gpp_a_1_prt_p3 FOR VALUES FROM (20) TO (30); gpp_a_1_prt_p4 FOR VALUES FROM (30) TO (35); gpp_a_pg FOR VALUES FROM (40) TO (50)"
# Cloudberry's own rule: with a partition starting where the new one ends,
# the START it finds is MINVALUE, and the partition overlaps.
refused "and one ending where another starts cannot be placed" \
        "ALTER TABLE gpp_a ADD PARTITION p5 END (40)" 'partition "gpp_a_1_prt_p5" would overlap partition "gpp_a_1_prt_p1"'
q "INSERT INTO gpp_a SELECT i, i FROM generate_series(0, 34) i;" > /dev/null
is "TRUNCATE PARTITION empties one" \
   "ALTER TABLE gpp_a TRUNCATE PARTITION FOR (5); SELECT count(*) FROM gpp_a;" "25"
is "SPLIT PARTITION ... AT: two in its place, and its rows put back through the table" \
   "ALTER TABLE gpp_a SPLIT PARTITION p2 AT (15) INTO (PARTITION p2a, PARTITION p2b);
    SELECT string_agg(tableoid::regclass || ':' || count, ' ' ORDER BY 1) FROM
      (SELECT tableoid, count(*) FROM gpp_a WHERE b BETWEEN 10 AND 19 GROUP BY 1) s;" \
   "gpp_a_1_prt_p2a:5 gpp_a_1_prt_p2b:5"
is "RENAME PARTITION" \
   "ALTER TABLE gpp_a RENAME PARTITION p2a TO early; SELECT to_regclass('gpp_a_1_prt_early') IS NOT NULL;" "t"
q "CREATE TABLE gpp_x (a int, b int); INSERT INTO gpp_x VALUES (1, 31), (2, 32);" > /dev/null
is "EXCHANGE PARTITION swaps a table in: the partition's name is the table's now, and the rows" \
   "ALTER TABLE gpp_a EXCHANGE PARTITION p4 WITH TABLE gpp_x;
    SELECT (SELECT count(*) FROM gpp_a_1_prt_p4), (SELECT count(*) FROM gpp_x);" "2|5"
refused "DROP PARTITION of one that is not there" \
        "ALTER TABLE gpp_a DROP PARTITION nosuch" 'relation "public.gpp_a_1_prt_nosuch" does not exist'
is "and with IF EXISTS, nothing" \
   "ALTER TABLE gpp_a DROP PARTITION IF EXISTS nosuch; ALTER TABLE gpp_a DROP PARTITION p1;
    SELECT count(*) FROM pg_inherits WHERE inhparent = 'gpp_a'::regclass;" "5"
# A name too long to fit in <parent>_<level>_prt_<name> is no partition's:
# makeObjectName, which makes that name, asserts on one, and Cloudberry's
# lookup by name does not check for it.
long=$(printf 'p%.0s' $(seq 57))
refused "a name longer than a partition's can be names none" \
        "ALTER TABLE gpp_a TRUNCATE PARTITION $long" "partition \"$long\" of \"gpp_a\" does not exist"
is "and DROP PARTITION IF EXISTS of it does nothing" \
   "ALTER TABLE gpp_a DROP PARTITION IF EXISTS $long;
    SELECT count(*) FROM pg_inherits WHERE inhparent = 'gpp_a'::regclass;" "5"
refused "nor can a new partition be given one" \
        "ALTER TABLE gpp_a SPLIT PARTITION FOR (31) AT (33) INTO (PARTITION $long, PARTITION p4b)" \
        "name \"$long\" for child partition is too long"
refused "FOR (value) of the default partition's is refused" \
        "ALTER TABLE gpp_r DROP PARTITION FOR (date '1999-01-01')" \
        'FOR expression matches DEFAULT partition for specified value of relation "gpp_r"'
refused "and the last partition cannot go" \
        "CREATE TABLE gpp_one (a int) PARTITION BY RANGE (a) (START (1) END (2));
         ALTER TABLE gpp_one DROP PARTITION FOR (1);" \
        'cannot drop partition "gpp_one_1_prt_1" of "gpp_one" -- only one remains'
refused "a partition command on a table that is not partitioned" \
        "ALTER TABLE gpp_x ADD PARTITION p START (1) END (2)" 'table "gpp_x" is not partitioned'

q "ALTER TABLE gpp_s ADD PARTITION eu VALUES ('eu');" > /dev/null
isparts "ADD PARTITION of a table with a template makes the new partition's partitions from it" gpp_s_1_prt_eu \
   "gpp_s_1_prt_eu_2_prt_1 FOR VALUES FROM ('2020-01-01') TO ('2020-02-01'); gpp_s_1_prt_eu_2_prt_2 FOR VALUES FROM ('2020-02-01') TO ('2020-03-01')"
q "ALTER TABLE gpp_s ALTER PARTITION usa ADD PARTITION march START (date '2020-03-01') END (date '2020-04-01');" > /dev/null
is "ALTER PARTITION goes down to the partition its command is for" \
   "SELECT pg_get_expr(relpartbound, oid) FROM pg_class WHERE relname = 'gpp_s_1_prt_usa_2_prt_march';" \
   "FOR VALUES FROM ('2020-03-01') TO ('2020-04-01')"
is "SET SUBPARTITION TEMPLATE replaces it, and () takes it away" \
   "ALTER TABLE gpp_s SET SUBPARTITION TEMPLATE (START (date '2021-01-01') END (date '2021-02-01'));
    ALTER TABLE gpp_s ADD PARTITION cn VALUES ('cn');
    SELECT string_agg(pg_get_expr(relpartbound, oid), ' ') FROM pg_class WHERE relname LIKE 'gpp_s_1_prt_cn_2_prt%';
    ALTER TABLE gpp_s SET SUBPARTITION TEMPLATE ();
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'gpp_s'::regclass;" \
   "FOR VALUES FROM ('2021-01-01') TO ('2021-02-01')
0"
refused "and there is none to take away then" \
        "ALTER TABLE gpp_s SET SUBPARTITION TEMPLATE ()" \
        'relation "gpp_s" does not have a level 1 subpartition template specification'
is "renaming the table renames its partitions, as Cloudberry's RENAME does" \
   "ALTER TABLE gpp_i RENAME TO gpp_i2;
    SELECT string_agg(relname, ' ' ORDER BY relname) FROM pg_class WHERE relname LIKE 'gpp_i2%';" \
   "gpp_i2 gpp_i2_1_prt_a gpp_i2_1_prt_b gpp_i2_1_prt_c gpp_i2_1_prt_d"
is "and one made with PostgreSQL's syntax renames as PostgreSQL does" \
   "CREATE TABLE gpp_pg (a int) PARTITION BY RANGE (a);
    CREATE TABLE gpp_pg_1_part PARTITION OF gpp_pg FOR VALUES FROM (1) TO (2);
    ALTER TABLE gpp_pg RENAME TO gpp_pg2;
    SELECT string_agg(relname, ' ' ORDER BY relname) FROM pg_class WHERE relname LIKE 'gpp_pg%';" \
   "gpp_pg2 gpp_pg_1_part"

# --- one statement for one ------------------------------------------------------

answers "CREATE TABLE with the classic clause, prepared" \
        "CREATE TABLE gpp_b (a int) PARTITION BY RANGE (a) (START (1) END (3) EVERY (1))" "CREATE TABLE"
answers "and ALTER TABLE's commands" \
        "ALTER TABLE gpp_b ADD PARTITION START (3) END (4), DROP PARTITION FOR (1)" "ALTER TABLE"
isparts "which did what they say" gpp_b \
   "gpp_b_1_prt_11 FOR VALUES FROM (3) TO (4); gpp_b_1_prt_2 FOR VALUES FROM (2) TO (3)"

echo
echo "16. gp_dist_random('t'), the port's gp.dist_random(NULL::t)"
# Cloudberry's parser makes gp_dist_random('t') in FROM the relation t read
# on every segment, named t unless it has an alias (parse_clause.c).  The
# rewrite makes it gp.dist_random(NULL::t) AS t.  This is one node, where
# Cloudberry reads the relation here, and so does the port: gp_segment_id -1.

q "CREATE TABLE gdr (a int, b text); INSERT INTO gdr VALUES (1, 'one'), (2, 'two');
   CREATE SCHEMA \"Gdr S\"; CREATE TABLE \"Gdr S\".\"T x\" (a int); INSERT INTO \"Gdr S\".\"T x\" VALUES (7);" >/dev/null
is "gp_dist_random('t') in FROM is gp.dist_random(NULL::t), named t" \
   "SELECT gp_sql.desugar('SELECT gp_segment_id, * FROM gp_dist_random(''gdr'') WHERE a = 1');" \
   "SELECT gp_segment_id, * FROM gp.dist_random(NULL::gdr) AS gdr WHERE a = 1"
is "one node reads it here, and each row's gp_segment_id is -1" \
   "SELECT gp_segment_id, gdr.a, b FROM gp_dist_random('gdr') ORDER BY a;" "-1|1|one
-1|2|two"
is "an alias is the call's, AS or bare, after a comma and a JOIN too" \
   "SELECT gp_sql.desugar('SELECT 1 FROM gdr, gp_dist_random(''gdr'') AS x JOIN gp_dist_random(''gdr'') y USING (a)');" \
   "SELECT 1 FROM gdr, gp.dist_random(NULL::gdr) AS x JOIN gp.dist_random(NULL::gdr) y USING (a)"
is "a qualified name is taken apart as Cloudberry takes it, quoted parts and all" \
   "SELECT gp_sql.desugar('SELECT * FROM gp_dist_random(''\"Gdr S\".\"T x\"'')'), (SELECT a FROM gp_dist_random('\"Gdr S\".\"T x\"'));" \
   "SELECT * FROM gp.dist_random(NULL::\"Gdr S\".\"T x\") AS \"T x\"|7"
is "outside FROM, and with a decoration, it is left for PostgreSQL" \
   "SELECT gp_sql.desugar('SELECT gp_dist_random(''gdr'')') || ' / ' ||
           gp_sql.desugar('SELECT * FROM gp_dist_random(''gdr'') WITH ORDINALITY');" \
   "SELECT gp_dist_random('gdr') / SELECT * FROM gp_dist_random('gdr') WITH ORDINALITY"
at "a relation that does not exist is reported at the string" \
   "SELECT * FROM gp_dist_random('gdr_nosuch')" "does not exist" "'gdr_nosuch'"
refused "and a name that is not one, as Cloudberry reports it" \
   "SELECT * FROM gp_dist_random('a.b.c.d');" "improper qualified name (too many dotted names)"
is "a view of it keeps working" \
   "CREATE VIEW gdr_v AS SELECT gp_segment_id, a FROM gp_dist_random('gdr');
    SELECT count(*), min(gp_segment_id) FROM gdr_v;" "2|-1"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
