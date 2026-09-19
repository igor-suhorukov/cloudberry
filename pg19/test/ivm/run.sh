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

# same <label> <what the view says> <the same thing computed fresh>
#		A view is right when it holds neither more nor less than its own
#		query would answer now.
same() {
	is "$1" "SELECT count(*) FROM (($2 EXCEPT ALL $3) UNION ALL ($3 EXCEPT ALL $2)) d;" "0"
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
echo "3c. a view over two tables is maintained by delta"
###############################################################################
# On tables of their own, so that the counters below count only these views.
# This is what the pre-update state is for: one table's delta has to be joined
# against the other as it was before the statement, and the statement has
# already changed it by the time the trigger runs.
JOIN_FRESH="SELECT l.grp, r.tag FROM j1 l, j2 r WHERE l.grp = r.grp"
q "CREATE TABLE j1 (id int, grp int, amt numeric);
   CREATE TABLE j2 (grp int, tag text);
   INSERT INTO j1 VALUES (1,1,10),(2,2,20);
   INSERT INTO j2 VALUES (1,'a'),(2,'b');
   CREATE MATERIALIZED VIEW mv_join WITH (gp.incremental) AS
     SELECT l.grp, r.tag FROM j1 l, j2 r WHERE l.grp = r.grp;" > /dev/null
same "it starts out saying what the join says" "SELECT grp, tag FROM mv_join" "$JOIN_FRESH"

isl "changing one side is a delta, not a recomputation" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO j1 VALUES (3,2,3);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and the view still says what the join says" "SELECT grp, tag FROM mv_join" "$JOIN_FRESH"

isl "so is changing the other side" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO j2 VALUES (2,'b2');
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and it still does" "SELECT grp, tag FROM mv_join" "$JOIN_FRESH"

# Both tables in one statement: the case the pre-update state exists for.  The
# view is maintained once, from one delta per table, and the rows the two
# additions make with each other must be counted once rather than twice.
isl "a statement that changes both sides at once maintains the view once" \
    "SELECT gp_matview.stats_reset();
     WITH ins AS (INSERT INTO j1 VALUES (4,7,9) RETURNING grp)
       INSERT INTO j2 SELECT 7, 'c' FROM ins;
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and the row they make together appears exactly once" \
     "SELECT grp, tag FROM mv_join" "$JOIN_FRESH"

isl "the same when both sides lose rows at once" \
    "SELECT gp_matview.stats_reset();
     WITH del AS (DELETE FROM j2 WHERE tag = 'c' RETURNING grp)
       DELETE FROM j1 WHERE id = 4;
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and nothing of them is left behind" "SELECT grp, tag FROM mv_join" "$JOIN_FRESH"

isl "an UPDATE on one side, which is a delta both ways at once" \
    "SELECT gp_matview.stats_reset();
     UPDATE j2 SET grp = 1 WHERE tag = 'b2';
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "moves the rows it should and no others" "SELECT grp, tag FROM mv_join" "$JOIN_FRESH"

# A row trigger on one base table that writes another one.  The second table's
# AFTER trigger fires inside a nested query, which ends -- and frees what it
# left behind -- before the outer statement's own trigger runs.  So the view is
# maintained from something the module took out of that query rather than from
# the query's own transition table.
K_FRESH="SELECT a.grp, b.tag FROM k1 a, k2 b WHERE a.grp = b.grp"
q "CREATE TABLE k1 (id int, grp int);
   CREATE TABLE k2 (grp int, tag text);
   INSERT INTO k1 VALUES (1,1);
   INSERT INTO k2 VALUES (1,'x');
   CREATE FUNCTION mirror() RETURNS trigger LANGUAGE plpgsql AS \$\$
     BEGIN INSERT INTO k2 VALUES (NEW.grp, 'auto'); RETURN NEW; END \$\$;
   CREATE TRIGGER mirror AFTER INSERT ON k1
     FOR EACH ROW EXECUTE FUNCTION mirror();
   CREATE MATERIALIZED VIEW mv_k WITH (gp.incremental) AS
     SELECT a.grp, b.tag FROM k1 a, k2 b WHERE a.grp = b.grp;" > /dev/null
isl "a base table written from a row trigger is still a delta" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO k1 VALUES (2,1);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and the rows both changes make are all there, once each" \
     "SELECT grp, tag FROM mv_k" "$K_FRESH"
q "DROP MATERIALIZED VIEW mv_k; DROP TABLE k1; DROP TABLE k2; DROP FUNCTION mirror();" > /dev/null

# A table joined to itself is two places in one query, and each gets its own
# delta against the state the other has not reached yet.
SELF_FRESH="SELECT a.id AS x, b.id AS y FROM j1 a, j1 b WHERE a.grp = b.grp"
q "CREATE MATERIALIZED VIEW mv_self WITH (gp.incremental) AS
     SELECT a.id AS x, b.id AS y FROM j1 a, j1 b WHERE a.grp = b.grp;
   DROP MATERIALIZED VIEW mv_join;" > /dev/null
isl "a self-join is maintained by delta" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO j1 VALUES (5,1,2);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and the pair the new row makes with itself appears once" \
     "SELECT x, y FROM mv_self" "$SELF_FRESH"
q "DELETE FROM j1 WHERE id = 5;" > /dev/null
same "and goes again when the row does" "SELECT x, y FROM mv_self" "$SELF_FRESH"
q "DROP MATERIALIZED VIEW mv_self;" > /dev/null

# Counts and sums over a join: the delta has to carry those across too.
JA_FRESH="SELECT r.tag, count(*), sum(l.amt) FROM j1 l, j2 r WHERE l.grp = r.grp GROUP BY r.tag"
q "CREATE MATERIALIZED VIEW mv_ja WITH (gp.incremental) AS
     SELECT r.tag, count(*) AS n, sum(l.amt) AS total
       FROM j1 l, j2 r WHERE l.grp = r.grp GROUP BY r.tag;" > /dev/null
isl "an aggregate over a join is a delta as well" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO j1 VALUES (6,1,100);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and its counts and sums are right" "SELECT tag, n, total FROM mv_ja" "$JA_FRESH"
q "DELETE FROM j1 WHERE id = 6;" > /dev/null
same "still right after a delete" "SELECT tag, n, total FROM mv_ja" "$JA_FRESH"
q "DROP MATERIALIZED VIEW mv_ja;" > /dev/null

###############################################################################
echo "3f. avg() is maintained, from the sum and count beside it"
###############################################################################
AVG_FRESH="SELECT grp, avg(amt) FROM av GROUP BY grp"
q "CREATE TABLE av (id int, grp int, amt numeric);
   INSERT INTO av VALUES (1,1,10),(2,1,20),(3,2,30);
   CREATE MATERIALIZED VIEW mv_avg WITH (gp.incremental) AS
     SELECT grp, avg(amt) AS a FROM av GROUP BY grp;" > /dev/null
is "it is stored with a sum and a count of its own" \
   "SELECT count(*) FROM pg_attribute WHERE attrelid = 'mv_avg'::regclass
      AND attname IN ('__ivm_sum_a__', '__ivm_count_a__');" "2"
same "and it starts out right" "SELECT grp, a FROM mv_avg" "$AVG_FRESH"
isl "a change to its base table is a delta" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO av VALUES (4,1,4);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "1/0"
same "and the average is right" "SELECT grp, a FROM mv_avg" "$AVG_FRESH"
q "DELETE FROM av WHERE id = 4;" > /dev/null
same "and right again when the row goes" "SELECT grp, a FROM mv_avg" "$AVG_FRESH"
q "UPDATE av SET amt = 40 WHERE id = 1;" > /dev/null
same "and after an UPDATE" "SELECT grp, a FROM mv_avg" "$AVG_FRESH"
q "INSERT INTO av VALUES (5,9,NULL),(6,9,10),(7,9,20);" > /dev/null
same "a NULL input is left out, as avg() leaves it out" "SELECT grp, a FROM mv_avg" "$AVG_FRESH"
q "DELETE FROM av WHERE id IN (6,7);" > /dev/null
is "and with only the NULL left the average is NULL, not zero" \
   "SELECT a IS NULL FROM mv_avg WHERE grp = 9;" "t"
q "DELETE FROM av WHERE grp = 9;" > /dev/null
is "a group whose rows all go leaves no row behind" \
   "SELECT count(*) FROM mv_avg WHERE grp = 9;" "0"
q "DROP MATERIALIZED VIEW mv_avg; DROP TABLE av;" > /dev/null

###############################################################################
echo "3g. what the delta still cannot express is recomputed, and still correct"
###############################################################################
# Each of these leaves the view correct.  What it does not have is the cost of
# an incremental view, which is why it is the counters that are asserted.
OUTER_FRESH="SELECT l.grp, r.tag FROM j1 l LEFT JOIN j2 r ON l.grp = r.grp"
q "CREATE MATERIALIZED VIEW mv_outer WITH (gp.incremental) AS
     SELECT l.grp, r.tag FROM j1 l LEFT JOIN j2 r ON l.grp = r.grp;" > /dev/null
# An outer join's delta is not this arithmetic: losing the last matching row
# turns the inner columns null instead of removing a view row.
isl "an outer join is recomputed" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO j1 VALUES (7,2,1);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "0/1"
same "and it is still correct" "SELECT grp, tag FROM mv_outer" "$OUTER_FRESH"
q "DELETE FROM j2 WHERE grp = 2;" > /dev/null
same "including when a row loses its last match" "SELECT grp, tag FROM mv_outer" "$OUTER_FRESH"
q "DROP MATERIALIZED VIEW mv_outer;" > /dev/null

MM_FRESH="SELECT grp, min(amt), max(amt) FROM j1 GROUP BY grp"
q "CREATE MATERIALIZED VIEW mv_mm WITH (gp.incremental) AS
     SELECT grp, min(amt) AS lo, max(amt) AS hi FROM j1 GROUP BY grp;" > /dev/null
# When the row holding the extreme goes, the new extreme is somewhere in the
# group and only the base table knows where.  Cloudberry refuses such a view
# outright; here it is made, and recomputed.
isl "min() and max() are recomputed" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO j1 VALUES (8,1,1000);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "0/1"
same "and they are still correct" "SELECT grp, lo, hi FROM mv_mm" "$MM_FRESH"
q "DELETE FROM j1 WHERE id = 8;" > /dev/null
same "including after the row holding the largest value goes" \
     "SELECT grp, lo, hi FROM mv_mm" "$MM_FRESH"
q "DROP MATERIALIZED VIEW mv_mm; DROP TABLE j1; DROP TABLE j2;" > /dev/null

# A dropped column keeps its place in the table but loses its type, so a
# subquery standing in for the table cannot reproduce the column numbering the
# view was built against.  Reading the column next door would be worse than
# being slow, so such a view is recomputed.
q "CREATE TABLE holes (a int, junk int, b int);
   INSERT INTO holes VALUES (1,1,10),(2,2,20);
   CREATE MATERIALIZED VIEW mv_holes WITH (gp.incremental) AS
     SELECT a, b FROM holes;
   ALTER TABLE holes DROP COLUMN junk;" > /dev/null
is "a column no view reads can still be dropped" \
   "SELECT count(*) FROM pg_attribute
      WHERE attrelid = 'holes'::regclass AND attname = 'junk';" "0"
isl "and the view over that table is recomputed from then on" \
    "SELECT gp_matview.stats_reset();
     INSERT INTO holes VALUES (3,30);
     SELECT gp_matview.applied_delta() || '/' || gp_matview.recomputed();" "0/1"
same "reading the columns it was built against, not the ones beside them" \
     "SELECT a, b FROM mv_holes" "SELECT a, b FROM holes"
q "DROP MATERIALIZED VIEW mv_holes; DROP TABLE holes;" > /dev/null

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
# The first of these used to pass for the wrong reason: the statement failed
# with "unrecognized parameter namespace", no view was made, and a count of
# labels on a view that does not exist is also zero.  So ask whether the view
# is there before asking what it is.
q "CREATE MATERIALIZED VIEW mv3 WITH (gp.incremental = false) AS SELECT grp FROM base;" > /dev/null
is "WITH (gp.incremental = false) makes a view at all" \
   "SELECT count(*) FROM pg_class WHERE relname = 'mv3' AND relkind = 'm';" "1"
is "and it is an ordinary one" \
   "SELECT count(*) FROM pg_seclabels WHERE objname = 'mv3' AND provider = 'gp';" "0"
is "with no counter column" \
   "SELECT count(*) FROM pg_attribute
      WHERE attrelid = 'mv3'::regclass AND attname = '__ivm_count__';" "0"
is "and no triggers on its base table" \
   "SELECT count(*) FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid
     WHERE c.relname = 'base' AND t.tgname LIKE '%' || 'mv3';" "0"
# The same trap as above: a trigger count of zero is also what a view that was
# never created gives, so establish the view first.
q "CREATE MATERIALIZED VIEW mv4 AS SELECT grp FROM base;" > /dev/null
is "no option at all makes a view too" \
   "SELECT count(*) FROM pg_class WHERE relname = 'mv4' AND relkind = 'm';" "1"
is "and it is ordinary as well" \
   "SELECT count(*) FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid
     WHERE c.relname = 'base' AND t.tgname LIKE '%' || 'mv4';" "0"
q "CREATE MATERIALIZED VIEW mv5 WITH (gp.incremental = true) AS
     SELECT grp, count(*) AS n FROM base GROUP BY grp;" > /dev/null
is "and WITH (gp.incremental = true) says it the long way" \
   "SELECT count(*) FROM pg_seclabels
      WHERE objname = 'mv5' AND provider = 'gp' AND label = 'incremental';" "1"
# It goes again straight away: it is an incremental view over the same base
# table, so its triggers would otherwise be counted by section 8.
q "DROP MATERIALIZED VIEW mv5;" > /dev/null
refused "a value that is not a boolean is refused, and says so" \
        "CREATE MATERIALIZED VIEW x WITH (gp.incremental = 'banana') AS SELECT 1;" \
        "requires a Boolean value"
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

###############################################################################
echo "10. the statement is not scribbled on when it is not ours to scribble on"
###############################################################################
# Taking the option out and rewriting the query both change the statement, and
# three callers of ProcessUtility pass readOnlyTree because the tree belongs to
# a plan cache: SPI (so PL/pgSQL), a SQL-language function, and a portal with a
# cached plan.  The module copies first in that case.
#
# What these tests can and cannot show.  They cannot show the bug the copy
# prevents: BuildCachedPlan copies query_list for any saved plan
# (pg19/src/backend/utils/cache/plancache.c:1072-1082), and DDL invalidates
# cached plans anyway, so for CREATE MATERIALIZED VIEW the tree handed to the
# hook is freshly parsed every time and a scribble never survives to be seen.
# What they do show is that the copy is right -- a copy whose ctas pointer was
# not re-derived, which is the easy mistake here, fails all of these.

q "CREATE TABLE b10 (id int, grp int);
   INSERT INTO b10 VALUES (1,1),(2,1),(3,2);
   CREATE FUNCTION mk10() RETURNS void LANGUAGE plpgsql AS \$\$
   BEGIN
     DROP MATERIALIZED VIEW IF EXISTS m10;
     CREATE MATERIALIZED VIEW m10 WITH (gp.incremental) AS
       SELECT grp, count(*) AS n FROM b10 GROUP BY grp;
   END \$\$;" > /dev/null

for n in 1 2 3; do
	q "SELECT mk10();" > /dev/null
	is "through PL/pgSQL, call $n leaves an incremental view" \
	   "SELECT count(*) FROM pg_seclabels
	      WHERE objname = 'm10' AND provider = 'gp' AND label = 'incremental';" "1"
done

is "and it is maintained, so the rewrite reached the right tree" \
   "SELECT n FROM m10 WHERE grp = 1;" "2"
q "INSERT INTO b10 VALUES (4,1);" > /dev/null
is "still maintained after the last copy" \
   "SELECT n FROM m10 WHERE grp = 1;" "3"

q "CREATE FUNCTION mksql10() RETURNS void LANGUAGE sql AS \$\$
     CREATE MATERIALIZED VIEW s10 WITH (gp.incremental) AS
       SELECT grp, count(*) AS n FROM b10 GROUP BY grp;
   \$\$;" > /dev/null
q "SELECT mksql10();" > /dev/null
is "through a SQL-language function too" \
   "SELECT count(*) FROM pg_seclabels
      WHERE objname = 's10' AND provider = 'gp' AND label = 'incremental';" "1"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
