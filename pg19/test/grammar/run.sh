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

echo "O26: Cloudberry's SQL, rewritten into PostgreSQL 19's"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "shared_preload_libraries = 'gp_core,gp_task,gp_matview,gp_sql,gp_security'"
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
isl "UNSET TAG takes one off" \
   "ALTER TABLE tagged UNSET TAG (tier);
    SELECT gp_sql.relation_tags('tagged'::regclass)::text;" '{"env": "prod"}'
refused "a tag nobody defined is still refused" \
        "CREATE TABLE never_made (id int) TAG (nope = 'x');" \
        "tag \"nope\" does not exist"

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
is "where no statement has a place for them, the calls are one SELECT" \
   "SELECT gp_sql.desugar('ALTER SCHEMA s TAG (env = ''prod'', tier = ''gold'')')
           !~ ';' AND gp_sql.desugar('DROP TAG t1, t2') !~ ';';" "t"

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

# A policy that cannot be read is an error naming the problem, not a shrug.
# Cloudberry cannot reach the first of these -- its policy is a catalog row
# with dependencies, and it refuses to drop a distribution key column -- but
# the port has no such hook yet, so the case is real.
q "CREATE TABLE dist_drop (a int, b int) DISTRIBUTED BY (a, b);
   ALTER TABLE dist_drop DROP COLUMN b;" > /dev/null
refused "a key column that was dropped is named, not ignored" \
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
isl "DROP PROFILE" \
   "ALTER USER carol NOPROFILE;
    DROP PROFILE strict;
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
# of every function it meets.  The reader has existed since the compat layer
# landed; until now the label could only be set by hand, so the syntax was
# still a syntax error on the port.  The two halves are tested against the
# same strings: the ORCA suite asserts the reading, this one the writing.

isl "EXECUTE ON ALL SEGMENTS" \
   "CREATE FUNCTION xf1(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON ALL SEGMENTS;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf1(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=all_segments"

isl "EXECUTE ON ANY" \
   "CREATE FUNCTION xf2(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON ANY;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf2(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=any"

isl "EXECUTE ON COORDINATOR" \
   "CREATE FUNCTION xf3(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON COORDINATOR;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf3(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=coordinator"

# Cloudberry's older spelling, which its own grammar still accepts and maps to
# the same value.  A label a person reads says "coordinator" either way.
isl "EXECUTE ON MASTER, which means the same thing" \
   "CREATE FUNCTION xf4(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON MASTER;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf4(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=coordinator"

isl "EXECUTE ON INITPLAN" \
   "CREATE FUNCTION xf5(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON INITPLAN;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf5(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=initplan"

isl "ALTER FUNCTION changes it" \
   "ALTER FUNCTION xf1(int) EXECUTE ON COORDINATOR;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf1(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=coordinator"

# A DEFAULT belongs to CREATE FUNCTION and not to a signature: SECURITY LABEL
# takes a function_with_argtypes, which the grammar will not let one into.
isl "a parameter with a DEFAULT still finds its function" \
   "CREATE FUNCTION xf6(a int, b int DEFAULT 5) RETURNS int LANGUAGE sql
      AS 'SELECT a + b' EXECUTE ON ALL SEGMENTS;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf6(int,int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=all_segments"

isl "and so does an OUT parameter, which the lookup ignores" \
   "CREATE FUNCTION xf7(IN a int, OUT b int) LANGUAGE sql AS 'SELECT a'
      EXECUTE ON ALL SEGMENTS;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf7(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=all_segments"

isl "a type that is spelled with a comma inside parentheses" \
   "CREATE FUNCTION xf8(a numeric(10,2)) RETURNS int LANGUAGE sql AS 'SELECT 1'
      EXECUTE ON ALL SEGMENTS;
    SELECT label FROM pg_seclabel WHERE objoid = 'xf8(numeric)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=all_segments"

isl "a schema-qualified name" \
   "CREATE SCHEMA xs;
    CREATE FUNCTION xs.xf9(int) RETURNS int LANGUAGE sql AS 'SELECT \$1'
      EXECUTE ON ALL SEGMENTS;
    SELECT label FROM pg_seclabel WHERE objoid = 'xs.xf9(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=all_segments"

isl "a procedure, which shares the clause in Cloudberry's grammar" \
   "CREATE PROCEDURE xp1(int) LANGUAGE sql AS 'SELECT \$1' EXECUTE ON ANY;
    SELECT label FROM pg_seclabel WHERE objoid = 'xp1(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=any"

is "a function written without the clause carries no label" \
   "CREATE FUNCTION xf10(int) RETURNS int LANGUAGE sql AS 'SELECT \$1';
    SELECT count(*) FROM pg_seclabel WHERE objoid = 'xf10(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass;" "0"

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

is "and the clause is gone from what the parser is handed" \
   "SELECT gp_sql.desugar('CREATE FUNCTION g(int) RETURNS int LANGUAGE sql
      AS ''SELECT 1'' EXECUTE ON ALL SEGMENTS') LIKE '%SECURITY LABEL FOR gp ON FUNCTION g(int) IS ''execute_on=all_segments''%';" \
   "t"

is "a statement with no clause is handed back untouched" \
   "SELECT gp_sql.desugar('GRANT EXECUTE ON FUNCTION xf10(int) TO PUBLIC')
         = 'GRANT EXECUTE ON FUNCTION xf10(int) TO PUBLIC';" "t"

# Taking the clause out of an ALTER leaves no action, and ALTER FUNCTION will
# not accept that -- so the label replaces the statement.  Written beside
# another action the ALTER stays and does the rest.  Same shape as
# ALTER TABLE t TAG (...).
isl "ALTER FUNCTION with another action keeps the ALTER" \
   "ALTER FUNCTION xf2(int) STRICT EXECUTE ON ALL SEGMENTS;
    SELECT proisstrict::text || ' ' ||
           (SELECT label FROM pg_seclabel WHERE objoid = p.oid
              AND classoid = 'pg_proc'::regclass AND provider = 'gp')
      FROM pg_proc p WHERE p.oid = 'xf2(int)'::regprocedure;" \
   "true execute_on=all_segments"

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
    SELECT label FROM pg_seclabel WHERE objoid = 'da1()'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "data_access=none"

isl "CONTAINS SQL" \
   "CREATE FUNCTION da2(int) RETURNS int LANGUAGE sql CONTAINS SQL
      AS 'SELECT \$1';
    SELECT label FROM pg_seclabel WHERE objoid = 'da2(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "data_access=contains"

isl "READS SQL DATA" \
   "CREATE FUNCTION da3(int) RETURNS int LANGUAGE sql READS SQL DATA
      AS 'SELECT \$1';
    SELECT label FROM pg_seclabel WHERE objoid = 'da3(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "data_access=reads"

isl "MODIFIES SQL DATA" \
   "CREATE FUNCTION da4(int) RETURNS int LANGUAGE sql MODIFIES SQL DATA
      AS 'SELECT \$1';
    SELECT label FROM pg_seclabel WHERE objoid = 'da4(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "data_access=modifies"

# A procedure takes them too.
isl "a procedure takes them as well" \
   "CREATE PROCEDURE da5(int) LANGUAGE sql READS SQL DATA AS 'SELECT \$1';
    SELECT label FROM pg_seclabel WHERE objoid = 'da5(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "data_access=reads"

# --- Cloudberry's three rules ------------------------------------------------
#
# They are validate_sql_data_access() in its functioncmds.c, and they are the
# only thing the attribute does.  They are checked in the rewriter rather than
# in the label provider, because Cloudberry rejects the statement before the
# function is created and a check on the SECURITY LABEL that follows would
# reject it after -- leaving the function behind.
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

# The rejection has to happen before the function exists, which is the whole
# reason the check is in the rewriter.
is "and the rejected function was not created" \
   "SELECT count(*) FROM pg_proc WHERE proname LIKE 'dbad%';" "0"

isl "IMMUTABLE with CONTAINS SQL is allowed" \
   "CREATE FUNCTION da6(int) RETURNS int LANGUAGE sql IMMUTABLE CONTAINS SQL
      AS 'SELECT \$1';
    SELECT label FROM pg_seclabel WHERE objoid = 'da6(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "data_access=contains"

# --- composing with EXECUTE ON -----------------------------------------------
#
# This is why the two families are read in one pass.  SECURITY LABEL replaces
# a provider's label rather than merging into it, so a statement per clause
# would leave only the second key.
isl "both clauses on one function write both keys" \
   "CREATE FUNCTION da7(int) RETURNS int LANGUAGE sql READS SQL DATA
      EXECUTE ON ALL SEGMENTS AS 'SELECT \$1';
    SELECT label FROM pg_seclabel WHERE objoid = 'da7(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=all_segments,data_access=reads"

# The keys come out in a fixed order, so two spellings of the same function
# produce the same label rather than two that compare unequal.
isl "written the other way round, the label is the same" \
   "CREATE FUNCTION da8(int) RETURNS int LANGUAGE sql EXECUTE ON ALL SEGMENTS
      READS SQL DATA AS 'SELECT \$1';
    SELECT label FROM pg_seclabel WHERE objoid = 'da8(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "execute_on=all_segments,data_access=reads"

is "and the two agree" \
   "SELECT (SELECT label FROM pg_seclabel WHERE objoid = 'da7(int)'::regprocedure
              AND classoid = 'pg_proc'::regclass AND provider = 'gp')
         = (SELECT label FROM pg_seclabel WHERE objoid = 'da8(int)'::regprocedure
              AND classoid = 'pg_proc'::regclass AND provider = 'gp');" "t"

# --- ALTER -------------------------------------------------------------------
isl "ALTER FUNCTION can set it on its own" \
   "ALTER FUNCTION da2(int) MODIFIES SQL DATA;
    SELECT label FROM pg_seclabel WHERE objoid = 'da2(int)'::regprocedure
       AND classoid = 'pg_proc'::regclass AND provider = 'gp';" \
   "data_access=modifies"

isl "and beside another action the ALTER still does the rest" \
   "ALTER FUNCTION da3(int) STRICT CONTAINS SQL;
    SELECT proisstrict::text || ' ' ||
           (SELECT label FROM pg_seclabel WHERE objoid = p.oid
              AND classoid = 'pg_proc'::regclass AND provider = 'gp')
      FROM pg_proc p WHERE p.oid = 'da3(int)'::regprocedure;" \
   "true data_access=contains"

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
   "SELECT gp_sql.desugar('CREATE FUNCTION z() RETURNS int LANGUAGE plpgsql NO SQL AS ''x''') LIKE '%data_access=none%';" "t"

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
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
