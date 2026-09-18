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
# gp_matview: incrementally maintained materialized views.
#
# Cloudberry writes CREATE INCREMENTAL MATERIALIZED VIEW; here it is
# CREATE MATERIALIZED VIEW ... WITH (gp.incremental), which PostgreSQL's own
# grammar accepts and this module takes out before PostgreSQL would reject it.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/ivm/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-ivm-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbi-XXXXXX)"
PORT="${PGPORT:-$((6100 + RANDOM % 200))}"
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

q() { "$PSQL" -X -q -t -A -d postgres -c "$1" 2>&1; }

# is <label> <sql> <expected>
is() {
	local got; got=$(q "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

# isl <label> <sql> <expected>
#		The same, for a script whose answer is its last line.  The counters
#		below live in the backend, so resetting them, changing a table and
#		reading them back all have to be one session.
isl() {
	local got; got=$(q "$2" | grep -v '^$' | tail -1)
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

# refused <label> <sql> <text the error must contain>
refused() {
	local got; got=$(q "$2")
	case "$got" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$3], got [$got]" ;;
	esac
}

echo "gp_matview: incremental materialized views"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	# gp_matview reads CREATE MATERIALIZED VIEW before PostgreSQL does, so it
	# has to be loaded before any statement runs.
	echo "shared_preload_libraries = 'gp_core,gp_matview'"
} >> "$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

q "CREATE EXTENSION gp_matview CASCADE;" > /dev/null

###############################################################################
echo "1. an incremental view is made, marked and wired up"
###############################################################################
q "CREATE TABLE base (id int, grp int, amt numeric);
   INSERT INTO base VALUES (1,1,10),(2,1,20),(3,2,30);
   CREATE MATERIALIZED VIEW mv WITH (gp.incremental) AS
     SELECT grp, count(*) AS n, sum(amt) AS total FROM base GROUP BY grp;" > /dev/null

is "the view holds what the query says" \
   "SELECT string_agg(t::text, ' ' ORDER BY t::text) FROM (SELECT * FROM mv) t;" \
   "(1,2,30) (2,1,30)"
is "it carries the \"gp\" label that marks it incremental" \
   "SELECT label FROM pg_seclabels WHERE objtype = 'materialized view'
      AND objname = 'mv' AND provider = 'gp';" "incremental"
is "the hidden counter column is there" \
   "SELECT count(*) FROM pg_attribute
     WHERE attrelid = 'mv'::regclass AND attname = '__ivm_count__';" "1"
is "its base table carries the maintenance triggers" \
   "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'base'::regclass;" "8"

###############################################################################
echo "2. the counter column stays out of the way (O28)"
###############################################################################
is "SELECT * does not show it" \
   "SELECT count(*) FROM (SELECT * FROM mv) t
      CROSS JOIN LATERAL (SELECT 1) x WHERE false;" "0"
is "the view reads as three columns" \
   "SELECT array_length(string_to_array(trim(both '()' from (SELECT t::text FROM (SELECT * FROM mv) t LIMIT 1)), ','), 1);" "3"
is "but it is still there by name" \
   "SELECT count(DISTINCT __ivm_count__) > 0 FROM mv;" "t"

###############################################################################
echo "3. the view follows its base table"
###############################################################################
q "INSERT INTO base VALUES (4,2,5);" > /dev/null
is "after an INSERT" \
   "SELECT string_agg(t::text, ' ' ORDER BY t::text) FROM (SELECT * FROM mv) t;" \
   "(1,2,30) (2,2,35)"

q "DELETE FROM base WHERE id = 1;" > /dev/null
is "after a DELETE" \
   "SELECT string_agg(t::text, ' ' ORDER BY t::text) FROM (SELECT * FROM mv) t;" \
   "(1,1,20) (2,2,35)"

q "UPDATE base SET amt = 100 WHERE id = 2;" > /dev/null
is "after an UPDATE" \
   "SELECT string_agg(t::text, ' ' ORDER BY t::text) FROM (SELECT * FROM mv) t;" \
   "(1,1,100) (2,2,35)"

q "INSERT INTO base VALUES (5,3,1); DELETE FROM base WHERE grp = 3;" > /dev/null
is "a group that goes away leaves no row behind" \
   "SELECT count(*) FROM mv WHERE grp = 3;" "0"

q "TRUNCATE base;" > /dev/null
is "after TRUNCATE the view is empty" "SELECT count(*) FROM mv;" "0"

q "INSERT INTO base VALUES (1,1,7);" > /dev/null
is "and fills again" \
   "SELECT string_agg(t::text, ' ' ORDER BY t::text) FROM (SELECT * FROM mv) t;" \
   "(1,1,7)"

###############################################################################
echo "3b. the delta path is what did that, and it agrees with a fresh answer"
###############################################################################
# The view's contents do not say how they were produced, so the module counts.
isl "an INSERT is applied as a delta, not a recomputation" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO base VALUES (10,4,11),(11,4,12);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"

isl "and so are DELETE and UPDATE" \
    "SELECT gp_matview.stats_reset();
     DELETE FROM base WHERE grp = 4;
     UPDATE base SET amt = amt + 1;
     INSERT INTO base VALUES (12,5,1);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "3/0"

# The property that matters: the view says what the query would say.
is "the view equals the same query computed fresh" \
   "SELECT count(*) FROM (
      (SELECT grp, n, total FROM mv
       EXCEPT ALL
       SELECT grp, count(*), sum(amt) FROM base GROUP BY grp)
      UNION ALL
      (SELECT grp, count(*), sum(amt) FROM base GROUP BY grp
       EXCEPT ALL
       SELECT grp, n, total FROM mv)) d;" "0"

isl "TRUNCATE has no transition tables, so it recomputes instead" \
    "SELECT gp_matview.stats_reset();
     TRUNCATE base;
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "0/1"
q "INSERT INTO base VALUES (1,1,7);" > /dev/null

###############################################################################
echo "3c. what the delta cannot do yet is recomputed, and still correct"
###############################################################################
q "CREATE TABLE other (grp int, tag text);
   INSERT INTO other VALUES (1,'a'),(2,'b');
   CREATE MATERIALIZED VIEW mv_join WITH (gp.incremental) AS
     SELECT b.grp, o.tag FROM base b, other o WHERE b.grp = o.grp;" > /dev/null
isl "a view over two tables is recomputed, not delta'd" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO base VALUES (20,2,3);
     SELECT gp_matview.recomputed() > 0;" "t"
is "and it is still correct" \
   "SELECT count(*) FROM (
      (SELECT grp, tag FROM mv_join
       EXCEPT ALL
       SELECT b.grp, o.tag FROM base b, other o WHERE b.grp = o.grp)
      UNION ALL
      (SELECT b.grp, o.tag FROM base b, other o WHERE b.grp = o.grp
       EXCEPT ALL
       SELECT grp, tag FROM mv_join)) d;" "0"

q "CREATE MATERIALIZED VIEW mv_avg WITH (gp.incremental) AS
     SELECT grp, avg(amt) AS a FROM base GROUP BY grp;" > /dev/null
# avg() needs more than a count beside it, so such a view is recomputed.
isl "a view with avg() is recomputed, not delta'd" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO base VALUES (21,1,4);
     SELECT gp_matview.recomputed() > 0;" "t"
is "and it is still correct" \
   "SELECT count(*) FROM (
      (SELECT grp, a FROM mv_avg
       EXCEPT ALL
       SELECT grp, avg(amt) FROM base GROUP BY grp)
      UNION ALL
      (SELECT grp, avg(amt) FROM base GROUP BY grp
       EXCEPT ALL
       SELECT grp, a FROM mv_avg)) d;" "0"
q "DROP MATERIALIZED VIEW mv_avg; DROP MATERIALIZED VIEW mv_join; DROP TABLE other;" > /dev/null

###############################################################################
echo "3e. sum() is maintained, including when a group empties"
###############################################################################
isl "a sum is carried by the delta, not recomputed" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO base VALUES (30,7,5),(31,7,6);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
is "and it is right" "SELECT total FROM mv WHERE grp = 7;" "11"
q "DELETE FROM base WHERE grp = 7;" > /dev/null
is "a group whose rows all go leaves no row behind" \
   "SELECT count(*) FROM mv WHERE grp = 7;" "0"
q "INSERT INTO base VALUES (32,8,NULL),(33,8,4);" > /dev/null
is "a NULL input does not become part of the sum" \
   "SELECT total FROM mv WHERE grp = 8;" "4"
q "DELETE FROM base WHERE id = 33;" > /dev/null
is "and removing the only non-null input makes the sum NULL again" \
   "SELECT total IS NULL FROM mv WHERE grp = 8;" "t"
q "DELETE FROM base WHERE grp = 8;" > /dev/null

###############################################################################
echo "3d. a DISTINCT view counts duplicates"
###############################################################################
q "CREATE TABLE dup (a int);
   INSERT INTO dup VALUES (1),(1),(2);
   CREATE MATERIALIZED VIEW mv_d WITH (gp.incremental) AS SELECT DISTINCT a FROM dup;
   SELECT gp_matview.stats_reset();" > /dev/null
is "it starts with the distinct values" \
   "SELECT string_agg(a::text, ',' ORDER BY a) FROM mv_d;" "1,2"
q "DELETE FROM dup WHERE a = 1;" > /dev/null
is "removing every duplicate removes the row" \
   "SELECT string_agg(a::text, ',' ORDER BY a) FROM mv_d;" "2"
isl "adding a duplicate does not add a row" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO dup VALUES (2),(3);
     SELECT string_agg(a::text, ',' ORDER BY a) FROM mv_d;" "2,3"
isl "all of which was done with deltas" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO dup VALUES (4);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
q "DROP MATERIALIZED VIEW mv_d; DROP TABLE dup;" > /dev/null

###############################################################################
echo "4. maintenance mode is the module's, not the user's (O27)"
###############################################################################
refused "writing to the view directly is still refused" \
        "INSERT INTO mv VALUES (9, 9, 9);" "cannot change materialized view"
refused "and so is updating it" \
        "UPDATE mv SET n = 0;" "cannot change materialized view"

###############################################################################
echo "5. a view without aggregates needs no counter"
###############################################################################
q "CREATE MATERIALIZED VIEW mv2 WITH (gp.incremental) AS
     SELECT id, amt FROM base WHERE amt > 0;" > /dev/null
# Compared with the base table rather than a fixed number, so that what the
# sections before this one left behind does not matter.
is "it is made, and holds what the query says" \
   "SELECT (SELECT count(*) FROM mv2) = (SELECT count(*) FROM base WHERE amt > 0);" "t"
is "and has no hidden column" \
   "SELECT count(*) FROM pg_attribute
     WHERE attrelid = 'mv2'::regclass AND attname LIKE '\\_\\_ivm\\_%';" "0"
q "INSERT INTO base VALUES (99,1,3);" > /dev/null
is "it follows the base table too" \
   "SELECT (SELECT count(*) FROM mv2) = (SELECT count(*) FROM base WHERE amt > 0);" "t"

###############################################################################
echo "6. what incremental maintenance cannot express is refused"
###############################################################################
refused "ORDER BY" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS SELECT grp FROM base ORDER BY grp;" \
        "ORDER BY is not supported"
refused "LIMIT" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS SELECT grp FROM base LIMIT 1;" \
        "LIMIT or OFFSET is not supported"
refused "a subquery" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS SELECT (SELECT max(amt) FROM base);" \
        "subquery is not supported"
refused "a CTE" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS WITH c AS (SELECT 1 a) SELECT a FROM c;" \
        "is not supported"
refused "HAVING" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS
           SELECT grp, count(*) FROM base GROUP BY grp HAVING count(*) > 1;" \
        "HAVING is not supported"
refused "a window function" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS
           SELECT grp, row_number() OVER (ORDER BY grp) FROM base;" \
        "window function is not supported"
refused "UNION" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS
           SELECT grp FROM base UNION SELECT grp FROM base;" \
        "UNION, INTERSECT or EXCEPT is not supported"
refused "DISTINCT ON" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS
           SELECT DISTINCT ON (grp) grp, amt FROM base;" \
        "DISTINCT ON is not supported"
refused "a column named like the hidden ones" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental) AS
           SELECT grp AS __ivm_mine FROM base;" \
        "is reserved"

###############################################################################
echo "7. the option is an option, and an ordinary view is still ordinary"
###############################################################################
q "CREATE MATERIALIZED VIEW mv3 WITH (gp.incremental = false) AS SELECT grp FROM base;" > /dev/null
is "WITH (gp.incremental = false) makes an ordinary view" \
   "SELECT count(*) FROM pg_seclabels WHERE objname = 'mv3' AND provider = 'gp';" "0"
q "CREATE MATERIALIZED VIEW mv4 AS SELECT grp FROM base;" > /dev/null
is "and so does no option at all" \
   "SELECT count(*) FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid
     WHERE c.relname = 'base' AND t.tgname LIKE '%' || 'mv4';" "0"
refused "an unknown gp option is still rejected" \
        "CREATE MATERIALIZED VIEW x WITH (gp.nosuchthing) AS SELECT 1;" \
        "unrecognized parameter namespace"

###############################################################################
echo "8. dropping the view takes its triggers with it"
###############################################################################
q "DROP MATERIALIZED VIEW mv2;" > /dev/null
is "the triggers of the dropped view are gone" \
   "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'base'::regclass
      AND tgname LIKE '%_' || (SELECT oid FROM pg_class WHERE relname = 'mv');" "8"
q "DROP MATERIALIZED VIEW mv;" > /dev/null
is "and with the last view gone, so are all of them" \
   "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'base'::regclass;" "0"
is "the label went with it" \
   "SELECT count(*) FROM pg_seclabels WHERE objname = 'mv' AND provider = 'gp';" "0"

###############################################################################
echo "9. the label refuses what the port would not understand"
###############################################################################
refused "an unknown key" \
        "SECURITY LABEL FOR gp ON TABLE base IS 'nosuchkey';" \
        "unrecognized"
refused "a value on a flag" \
        "SECURITY LABEL FOR gp ON TABLE base IS 'incremental=yes';" \
        "takes no value"
refused "a flag where a value belongs" \
        "SECURITY LABEL FOR gp ON TABLE base IS 'execute_on';" \
        "needs a value"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
