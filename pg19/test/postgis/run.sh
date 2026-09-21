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
# PostGIS on one node, and the rewrite in front of ORCA for its indexable
# functions (decision 1; pg19/orca/postgis.c).
#
# PostGIS's spatial predicates carry a support function that gives the
# planner a bounding-box condition an index can answer.  ORCA has no such
# logic, so Cloudberry refuses them all.  The port asks the support function
# before ORCA is called and puts its answer into the query, so the tests that
# matter are differential: for every function that has the support function,
# ORCA's plan uses the index condition the planner's does, and ORCA's rows
# are the planner's.  The rest pin where the rewrite asks and where it does
# not, which is where the planner asks and where it does not.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# PostGIS is built into the image the Compose project makes; a server built
# some other way may not have it.
if [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/postgis.control" ] ||
   [ ! -f "$("$BINDIR/pg_config" --pkglibdir)/gp_orca.so" ]; then
	echo "PostGIS or gp_orca is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-postgis-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbg-XXXXXX)"
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
q2() { "$PSQL" -X -q -t -A -d postgres -c "$1" -c "$2" 2>&1; }

is() {
	local got; got=$(q "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

# The index conditions of a plan, one per line, sorted.  The planner is
# pushed off sequential scans, so that what it would ask for is in its plan
# rather than only in its choices; ORCA is left to choose.
PLANNER="SET gp.optimizer = off; SET enable_seqscan = off"
ORCA="SET gp.optimizer = on"
index_conds() {
	printf '%s\n' "$1" | sed -n 's/^ *\(->  \)*Index Cond: //p' | sort -u
}

# asks <name> <query>: ORCA planned it, with the planner's index conditions.
asks() {
	local plan orca pg
	plan=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) $2")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tr '\n' '|')"; return ;;
	esac
	orca=$(index_conds "$plan")
	pg=$(index_conds "$(q2 "$PLANNER" "EXPLAIN (COSTS OFF) $2")")
	if [ -n "$pg" ] && [ "$orca" = "$pg" ]; then
		ok "$1"
	else
		notok "$1" "orca [$(printf '%s' "$orca" | tr '\n' '|')], planner [$(printf '%s' "$pg" | tr '\n' '|')]"
	fi
}

# same <name> <query>: ORCA planned it, and answered as the planner does --
# the planner as it plans by itself, sequential scans and all.
same() {
	local plan orca pg
	plan=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) $2")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tr '\n' '|')"; return ;;
	esac
	orca=$(q2 "$ORCA" "$2")
	pg=$(q2 "SET gp.optimizer = off" "$2")
	[ "$orca" = "$pg" ] && ok "$1" || notok "$1" "orca [$orca], planner [$pg]"
}

# unasked <name> <query> <operator>: ORCA planned it, and nothing put the
# operator into its plan.
unasked() {
	local plan
	plan=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) $2")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tr '\n' '|')"; return ;;
	esac
	case "$plan" in
		*" $3 "*) notok "$1" "found [$3] in: $(printf '%s' "$plan" | tr '\n' '|')" ;;
		*) ok "$1" ;;
	esac
}

echo "postgis: PostGIS on one node, and ORCA's rewrite for its indexable functions"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	# Every module M1 has, as a user runs them, in the order cloudberry.md
	# gives: a module is only known to work beside the others once it has
	# been run beside them.
	echo "shared_preload_libraries = 'gp_core,gp_orca,gp_task,gp_matview,gp_sql,gp_security'"
} >> "$WORK/data/postgresql.conf"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

echo "1. PostGIS is stock, and installs beside the port's modules"

is "the image's PostGIS is the commit cloudberry.md cites" \
   "SELECT pg_read_file('$("$BINDIR/pg_config" --bindir)/../.postgis_commit') LIKE 'e2ea03ad0bfcf4b15ccb748113be3c5741809a08%';" "t"

# Every M1 module is preloaded, gp_sql's rewriter of Cloudberry's syntax
# among them, and PostGIS's scripts are 30,000 lines of SQL that pass through
# it.  That they install is the first thing worth knowing.
q "CREATE EXTENSION gp_orca CASCADE;" > /dev/null
for ext in postgis postgis_raster postgis_topology; do
	got=$(q "CREATE EXTENSION $ext;")
	[ -z "$got" ] && ok "CREATE EXTENSION $ext" || notok "CREATE EXTENSION $ext" "$got"
done

is "it is 3.7.0rc2" \
   "SELECT postgis_lib_version();" "3.7.0rc2"

# The functions the rewrite is for: every one that names PostGIS's support
# function.  cloudberry.md counted seventeen, which is the geometry ones;
# four geography overloads have it too.  If a PostGIS release adds one, this
# is the test that says so.
is "21 signatures carry postgis_index_supportfn: 17 over geometry, 4 over geography" \
   "SELECT count(*) FILTER (WHERE p.proargtypes[0] = 'geometry'::regtype) || ' ' ||
           count(*) FILTER (WHERE p.proargtypes[0] = 'geography'::regtype)
      FROM pg_proc p WHERE p.prosupport = 'postgis_index_supportfn'::regproc;" "17 4"

echo
echo "2. without the rewrite, ORCA refuses them, as Cloudberry's does"

q "CREATE TABLE pts (id int, geom geometry);
   INSERT INTO pts SELECT i, ST_MakePoint(i % 100, i / 100) FROM generate_series(0, 9999) i;
   INSERT INTO pts VALUES (10000, NULL), (10001, 'POINT EMPTY');
   CREATE INDEX pts_geom ON pts USING gist (geom);
   ANALYZE pts;" > /dev/null

W="ST_MakeEnvelope(10, 10, 20, 20)"

got=$(q2 "SET gp.optimizer_postgis_rewrite = off; SET gp.optimizer_trace_fallback = on" \
         "SELECT count(*) FROM pts WHERE ST_Intersects(geom, $W);")
case "$got" in
	*"extension functions with prosupport unsupported"*"121") ok "switched off, ORCA refuses with Cloudberry's reason, and the planner answers" ;;
	*) notok "switched off, ORCA refuses with Cloudberry's reason, and the planner answers" "$got" ;;
esac

got=$(q2 "SET gp.optimizer_postgis_rewrite = off" \
         "EXPLAIN (COSTS OFF) SELECT ST_AsText(geom) FROM pts WHERE id = 7;")
case "$got" in
	*"Optimizer: GPORCA"*) ok "a query calling no such function is ORCA's all the same" ;;
	*) notok "a query calling no such function is ORCA's all the same" "$got" ;;
esac

# Anywhere in the query, not only where an index could help: what is
# refused is the function, as Cloudberry's translator refuses it.
got=$(q2 "SET gp.optimizer_postgis_rewrite = off" \
         "EXPLAIN (COSTS OFF) SELECT ST_Intersects(geom, $W) FROM pts WHERE id = 7;")
case "$got" in
	*"Optimizer: Postgres query optimizer"*) ok "and a call in the target list is refused too" ;;
	*) notok "and a call in the target list is refused too" "$got" ;;
esac

echo
echo "3. every indexable function gets the planner's index condition, and its rows"

# Shapes of every kind, so that each predicate has rows to find: points on a
# grid, short lines, small squares; and a row with no geometry and one with
# an empty one, which each predicate has an answer for.
q "CREATE TABLE shapes (id int, geom geometry);
   INSERT INTO shapes SELECT i, ST_MakePoint(i % 100, i / 100) FROM generate_series(0, 9999) i;
   INSERT INTO shapes SELECT 10000 + i, ST_MakeLine(ST_MakePoint(i % 100, i / 100),
                                                    ST_MakePoint(i % 100 + 1.5, i / 100 + 0.5))
     FROM generate_series(0, 9999, 7) i;
   INSERT INTO shapes SELECT 20000 + i, ST_MakeEnvelope(i % 100, i / 100, i % 100 + 2, i / 100 + 2)
     FROM generate_series(0, 9999, 11) i;
   INSERT INTO shapes VALUES (30000, NULL), (30001, 'POINT EMPTY'), (30002, 'POLYGON EMPTY');
   CREATE INDEX shapes_geom ON shapes USING gist (geom);
   ANALYZE shapes;" > /dev/null

L="ST_MakeLine(ST_MakePoint(10, 9.5), ST_MakePoint(20, 20.5))"
P="ST_MakePoint(50, 50)"

# name|the call, over shapes.geom; each is tested with the indexed column
# first and, where the function takes it, second.
while IFS='|' read -r name call; do
	[ -z "$name" ] && continue
	asks "$name: ORCA's index condition is the planner's" \
	     "SELECT id FROM shapes WHERE $call"
	same "$name: and so are the rows" \
	     "SELECT string_agg(id::text, ',' ORDER BY id) FROM shapes WHERE $call"
done <<EOF
ST_Intersects|ST_Intersects(geom, $W)
ST_Intersects, index second|ST_Intersects($W, geom)
ST_DWithin|ST_DWithin(geom, $P, 3)
ST_DWithin, index second|ST_DWithin($P, geom, 3)
ST_Contains|ST_Contains($W, geom)
ST_Contains, index first|ST_Contains(geom, ST_MakePoint(30.5, 30.5))
ST_Within|ST_Within(geom, $W)
ST_Within, index second|ST_Within(ST_MakePoint(30.5, 30.5), geom)
ST_Touches|ST_Touches(geom, $W)
ST_3DIntersects|ST_3DIntersects(geom, $W)
ST_ContainsProperly|ST_ContainsProperly($W, geom)
ST_CoveredBy|ST_CoveredBy(geom, $W)
ST_Overlaps|ST_Overlaps(geom, $W)
ST_Covers|ST_Covers($W, geom)
ST_Crosses|ST_Crosses(geom, $L)
ST_DFullyWithin|ST_DFullyWithin(geom, $P, 3)
ST_DFullyWithin, index second|ST_DFullyWithin($P, geom, 3)
ST_3DDWithin|ST_3DDWithin(geom, $P, 3)
ST_3DDFullyWithin|ST_3DDFullyWithin(geom, $P, 3)
ST_OrderingEquals|ST_OrderingEquals(geom, ST_MakePoint(42, 17))
ST_Equals|ST_Equals(geom, ST_MakePoint(42, 17))
EOF

# The one that returns an integer: a comparison of it is an OpExpr, which
# the planner does not match to an index through the function's support
# function, so nothing is asked -- and ORCA plans the call as it is.
unasked "ST_LineCrossingDirection returns an integer, and is asked nothing" \
        "SELECT id FROM shapes WHERE ST_LineCrossingDirection(geom, $L) = 1" "&&"
same "and its rows are the planner's" \
     "SELECT string_agg(id::text, ',' ORDER BY id) FROM shapes WHERE ST_LineCrossingDirection(geom, $L) <> 0"

# ORCA used the index for these rather than only filtering by the condition:
# the condition is there to be probed with.
got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) SELECT id FROM shapes WHERE ST_Intersects(geom, $W)")
case "$got" in
	*"Index Scan using shapes_geom"*|*"Bitmap Index Scan on shapes_geom"*) ok "ORCA scans the GiST index for a selective predicate" ;;
	*) notok "ORCA scans the GiST index for a selective predicate" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac

echo
echo "4. geography, whose four overloads ask for the overlap strategy"

q "CREATE TABLE places (id int, geog geography);
   INSERT INTO places SELECT i, ST_MakePoint(i % 100 - 50, i / 100 - 50)::geography
     FROM generate_series(0, 9999) i;
   INSERT INTO places VALUES (10000, NULL);
   CREATE INDEX places_geog ON places USING gist (geog);
   ANALYZE places;" > /dev/null

G="ST_MakeEnvelope(-5, -5, 5, 5)::geography"
while IFS='|' read -r name call; do
	[ -z "$name" ] && continue
	asks "$name(geography): ORCA's index condition is the planner's" \
	     "SELECT id FROM places WHERE $call"
	same "$name(geography): and so are the rows" \
	     "SELECT string_agg(id::text, ',' ORDER BY id) FROM places WHERE $call"
done <<EOF
ST_Intersects|ST_Intersects(geog, $G)
ST_DWithin|ST_DWithin(geog, ST_MakePoint(0, 0)::geography, 300000)
ST_Covers|ST_Covers($G, geog)
ST_CoveredBy|ST_CoveredBy(geog, $G)
EOF

echo
echo "5. what decides the operator is the index, as PostGIS decides it"

# A 3-D index: its operator family's overlap is &&&, which a 3-D function
# may use and a 2-D one may not -- PostGIS refuses that pairing itself, and
# the rewrite asks and is refused in the same way.
q "CREATE TABLE pts3 AS SELECT id, ST_Force3D(geom) AS geom FROM pts WHERE geom IS NOT NULL;
   CREATE INDEX pts3_geom ON pts3 USING gist (geom gist_geometry_ops_nd);
   ANALYZE pts3;" > /dev/null
asks "ST_3DIntersects on a 3-D index is asked for &&&" \
     "SELECT id FROM pts3 WHERE ST_3DIntersects(geom, ST_Force3D($W))"
got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) SELECT id FROM pts3 WHERE ST_3DIntersects(geom, ST_Force3D($W))")
case "$got" in
	*"&&&"*) ok "which is the operator in ORCA's plan" ;;
	*) notok "which is the operator in ORCA's plan" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac
unasked "ST_Intersects on a 3-D index is given nothing, as PostGIS gives the planner nothing" \
        "SELECT id FROM pts3 WHERE ST_Intersects(geom, ST_Force3D($W))" "&&&"
same "and its rows are the planner's" \
     "SELECT count(*) FROM pts3 WHERE ST_Intersects(geom, ST_Force3D($W))"

# BRIN: its 2-D operator family has the overlap strategy, so the support
# function answers for it as for GiST.  ORCA plans no BRIN scan of a heap
# table -- not for a && written by hand either -- so the condition is a
# filter in its plan rather than an index condition, which is ORCA's choice
# and not the rewrite's.
q "CREATE TABLE ptsb AS SELECT * FROM pts ORDER BY id;
   CREATE INDEX ptsb_geom ON ptsb USING brin (geom);
   ANALYZE ptsb;" > /dev/null
got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) SELECT id FROM ptsb WHERE ST_Intersects(geom, $W)")
case "$got" in
	*"Filter: ((geom && "*"Optimizer: GPORCA"*) ok "a BRIN index is asked about as a GiST one is" ;;
	*) notok "a BRIN index is asked about as a GiST one is" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac
same "and its rows are the planner's" \
     "SELECT count(*) FROM ptsb WHERE ST_Intersects(geom, $W)"

# That family has no equality strategy, and asked for one PostGIS's support
# function raises rather than answering nothing -- a defect of PostGIS
# 3.7.0rc2's.  The planner asks it, so a query that uses ST_Equals on a
# BRIN-indexed column fails under the planner too; what matters here is that
# it fails the same way under ORCA, and not in some new way.
pg=$(q2 "SET gp.optimizer = off" "SELECT count(*) FROM ptsb WHERE ST_Equals(geom, ST_MakePoint(42, 17))")
orca=$(q2 "$ORCA" "SELECT count(*) FROM ptsb WHERE ST_Equals(geom, ST_MakePoint(42, 17))")
case "$pg" in
	*"no spatial operator found for 'st_equals'"*)
		[ "$orca" = "$pg" ] && ok "ST_Equals on a BRIN index fails as the planner fails, with PostGIS's error" \
			|| notok "ST_Equals on a BRIN index fails as the planner fails, with PostGIS's error" "orca [$orca], planner [$pg]" ;;
	*) notok "ST_Equals on a BRIN index fails as the planner fails, with PostGIS's error" "the planner did not fail: [$pg]" ;;
esac

echo
echo "6. a join, where the other table's column is what the index is probed with"

q "CREATE TABLE zones (id int, geom geometry);
   INSERT INTO zones SELECT i, ST_Buffer(ST_MakePoint(i % 10 * 10 + 5, i / 10 * 10 + 5), 2.5)
     FROM generate_series(0, 99) i;
   ANALYZE zones;" > /dev/null

asks "ST_Intersects across a join: the index on the points is probed with each zone" \
     "SELECT z.id, p.id FROM zones z JOIN pts p ON ST_Intersects(z.geom, p.geom)"
got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) SELECT z.id, p.id FROM zones z JOIN pts p ON ST_Intersects(z.geom, p.geom)")
case "$got" in
	*"Nested Loop"*"Index Scan using pts_geom"*|*"Nested Loop"*"Bitmap Index Scan on pts_geom"*) ok "and ORCA's plan is an index nested loop" ;;
	*) notok "and ORCA's plan is an index nested loop" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac
same "and the pairs are the planner's" \
     "SELECT count(*), sum(z.id * 100000 + p.id) FROM zones z JOIN pts p ON ST_Intersects(z.geom, p.geom)"
asks "ST_DWithin across a join: the radius expands the probing side" \
     "SELECT z.id, p.id FROM zones z JOIN pts p ON ST_DWithin(p.geom, ST_Centroid(z.geom), 1.5)"
same "and the pairs are the planner's" \
     "SELECT count(*), sum(z.id * 100000 + p.id) FROM zones z JOIN pts p ON ST_DWithin(p.geom, ST_Centroid(z.geom), 1.5)"
same "a LEFT JOIN keeps its zones with no points" \
     "SELECT count(*), count(p.id) FROM zones z LEFT JOIN pts p ON ST_Contains(z.geom, p.geom) AND p.id < 2000"
same "and an EXISTS subquery is asked about at its own level" \
     "SELECT count(*) FROM zones z WHERE EXISTS (SELECT 1 FROM pts p WHERE ST_Within(p.geom, z.geom) AND p.id % 3 = 0)"
same "as is a NOT EXISTS" \
     "SELECT count(*) FROM zones z WHERE NOT EXISTS (SELECT 1 FROM pts p WHERE ST_Within(p.geom, z.geom) AND p.id < 50)"

echo
echo "7. the column may be under a view, a subquery, a CTE or a UNION ALL"

q "CREATE VIEW pts_v AS SELECT id AS pid, geom AS g FROM pts WHERE id >= 0;
   CREATE TABLE pts2 AS SELECT id + 20000 AS id, geom FROM pts WHERE id < 5000;
   CREATE INDEX pts2_geom ON pts2 USING gist (geom);
   ANALYZE pts2;" > /dev/null

asks "through a view, to the index under it" \
     "SELECT pid FROM pts_v WHERE ST_Intersects(g, $W)"
same "and its rows are the planner's" \
     "SELECT string_agg(pid::text, ',' ORDER BY pid) FROM pts_v WHERE ST_Intersects(g, $W)"
asks "through a subquery in FROM" \
     "SELECT s.id FROM (SELECT id, geom FROM pts WHERE id >= 0) s WHERE ST_Within(s.geom, $W)"
asks "through a UNION ALL, to both branches' indexes" \
     "SELECT u.id FROM (SELECT id, geom FROM pts UNION ALL SELECT id, geom FROM pts2) u WHERE ST_Intersects(u.geom, $W)"
same "and its rows are the planner's" \
     "SELECT count(*), sum(u.id) FROM (SELECT id, geom FROM pts UNION ALL SELECT id, geom FROM pts2) u WHERE ST_Intersects(u.geom, $W)"
# ORCA keeps a CTE referenced once as a producer and consumer unless it
# inlines it; either way the condition reaches the plan, and the rows agree.
got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) WITH c AS (SELECT id, geom FROM pts) SELECT id FROM c WHERE ST_Intersects(geom, $W)")
case "$got" in
	*"Optimizer: GPORCA"*"&&"*|*"&&"*"Optimizer: GPORCA"*) ok "through a CTE, the condition is in ORCA's plan" ;;
	*) notok "through a CTE, the condition is in ORCA's plan" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac
same "and its rows are the planner's" \
     "WITH c AS (SELECT id, geom FROM pts) SELECT count(*), sum(id) FROM c WHERE ST_Intersects(geom, $W)"

echo
echo "8. where the planner asks nothing, neither does the rewrite"

q "CREATE TABLE pts_plain AS SELECT * FROM pts; ANALYZE pts_plain;
   CREATE TABLE pts_spg AS SELECT * FROM pts;
   CREATE INDEX pts_spg_geom ON pts_spg USING spgist (geom); ANALYZE pts_spg;
   CREATE TABLE pts_expr AS SELECT * FROM pts;
   CREATE INDEX pts_expr_geom ON pts_expr USING gist (ST_Force2D(geom)); ANALYZE pts_expr;
   CREATE TABLE pts_part AS SELECT * FROM pts;
   CREATE INDEX pts_part_geom ON pts_part USING gist (geom) WHERE id < 5000; ANALYZE pts_part;" > /dev/null

unasked "a column with no index" \
        "SELECT id FROM pts_plain WHERE ST_Intersects(geom, $W)" "&&"
unasked "a column whose only index is SP-GiST, which ORCA is not told about" \
        "SELECT id FROM pts_spg WHERE ST_Intersects(geom, $W)" "&&"
unasked "a partial index, which ORCA is not told about either" \
        "SELECT id FROM pts_part WHERE ST_Intersects(geom, $W)" "&&"
unasked "an expression index, nor that" \
        "SELECT id FROM pts_expr WHERE ST_Intersects(ST_Force2D(geom), $W)" "&&"
unasked "a call in the target list" \
        "SELECT ST_Intersects(geom, $W) FROM pts" "&&"
unasked "a call under NOT" \
        "SELECT id FROM pts WHERE NOT ST_Intersects(geom, $W)" "&&"
unasked "a call in a CASE" \
        "SELECT sum(CASE WHEN ST_Intersects(geom, $W) THEN 1 ELSE 0 END) FROM pts" "&&"
# PostGIS's own check: the other side must not read the indexed table.
unasked "a call whose other side reads the same table" \
        "SELECT id FROM pts WHERE ST_Intersects(geom, ST_Buffer(geom, 1))" "&&"
# The planner makes no index condition out of a function that is not
# leakproof under row-level security, and none of PostGIS's is.
q "CREATE TABLE pts_rls AS SELECT * FROM pts;
   CREATE INDEX pts_rls_geom ON pts_rls USING gist (geom);
   ALTER TABLE pts_rls ENABLE ROW LEVEL SECURITY;
   CREATE POLICY pts_rls_even ON pts_rls USING (id % 2 = 0);
   CREATE ROLE gis_reader; GRANT SELECT ON pts_rls TO gis_reader;
   ANALYZE pts_rls;" > /dev/null
got=$(q2 "$ORCA; SET ROLE gis_reader" "EXPLAIN (COSTS OFF) SELECT id FROM pts_rls WHERE ST_Intersects(geom, $W)")
case "$got" in
	*"Optimizer: GPORCA"*" && "*|*" && "*"Optimizer: GPORCA"*) notok "a table with row-level security" "$(printf '%s' "$got" | tr '\n' '|')" ;;
	*"Optimizer: GPORCA"*) ok "a table with row-level security" ;;
	*) notok "a table with row-level security" "not planned by ORCA: $(printf '%s' "$got" | tr '\n' '|')" ;;
esac
got=$(q2 "SET ROLE gis_reader" "SELECT count(*) FROM pts_rls WHERE ST_Intersects(geom, $W)")
[ "$got" = "$(q2 "SET ROLE gis_reader; SET gp.optimizer = off" "SELECT count(*) FROM pts_rls WHERE ST_Intersects(geom, $W)")" ] \
	&& ok "whose rows are the planner's" || notok "whose rows are the planner's" "$got"
same "and the rows of each are the planner's" \
     "SELECT (SELECT count(*) FROM pts WHERE NOT ST_Intersects(geom, $W)),
             (SELECT sum(CASE WHEN ST_Intersects(geom, $W) THEN 1 ELSE 0 END) FROM pts),
             (SELECT count(*) FROM pts_spg WHERE ST_Intersects(geom, $W))"

echo
echo "9. an OR, a condition written by hand, a prepared statement, an UPDATE"

# Each arm gains its condition.  ORCA scans rather than taking a BitmapOr
# here -- as it does for an OR of two && written by hand -- so what is
# checked is that both conditions are in its filter, each in its own arm.
got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) SELECT id FROM pts WHERE ST_Intersects(geom, $W) OR ST_DWithin(geom, $P, 2)")
case "$got" in
	*"((geom && "*"st_intersects("*") OR ((geom && st_expand("*"st_dwithin("*"Optimizer: GPORCA"*) ok "each arm of an OR is asked about, as for a BitmapOr" ;;
	*) notok "each arm of an OR is asked about, as for a BitmapOr" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac
same "and its rows are the planner's" \
     "SELECT count(*), sum(id) FROM pts WHERE ST_Intersects(geom, $W) OR ST_DWithin(geom, $P, 2)"

# PostGIS 2 put the && in by hand; a condition already in the conjunction is
# not added again, since ORCA would count it twice when estimating rows.
got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) SELECT id FROM pts WHERE geom && $W AND ST_Intersects(geom, $W)" |
      grep -o '&&' | wc -l)
got2=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) SELECT id FROM pts WHERE ST_Intersects(geom, $W)" |
      grep -o '&&' | wc -l)
[ "$got" = "$got2" ] && ok "a condition written by hand is not added again ($got && either way)" \
	|| notok "a condition written by hand is not added again" "by hand $got, rewritten $got2"

got=$(q2 "PREPARE near(geometry, float8) AS SELECT id FROM pts WHERE ST_DWithin(geom, \$1, \$2); SET plan_cache_mode = force_generic_plan; $ORCA" \
         "EXPLAIN (COSTS OFF) EXECUTE near('POINT(50 50)', 3)")
case "$got" in
	*"st_expand(\$1, \$2)"*"Optimizer: GPORCA"*) ok "a generic plan's parameters are what the radius expands" ;;
	*) notok "a generic plan's parameters are what the radius expands" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac

got=$(q2 "$ORCA" "EXPLAIN (COSTS OFF) UPDATE pts SET id = id WHERE ST_Intersects(geom, $W)")
case "$got" in
	*"Update on pts"*"Index Cond: (geom && "*"Optimizer: GPORCA"*) ok "an UPDATE finds its rows through the index" ;;
	*) notok "an UPDATE finds its rows through the index" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac

# A query with no range table and a support function in it is refused before
# constants are folded, for extensions whose support function reads the
# range table then; PostGIS's does not.
got=$(q2 "PREPARE nofrom(geometry, geometry) AS SELECT ST_Intersects(\$1, \$2); SET plan_cache_mode = force_generic_plan; $ORCA" \
         "EXPLAIN (COSTS OFF) EXECUTE nofrom('POINT(1 1)', 'POINT(1 1)')")
case "$got" in
	*"Optimizer: GPORCA"*) ok "a query with no FROM calling one is ORCA's" ;;
	*) notok "a query with no FROM calling one is ORCA's" "$(printf '%s' "$got" | tr '\n' '|')" ;;
esac

echo
echo "10. what is still refused, and what is counted"

# The exemption is PostGIS's support function, not every extension's: a
# function of anyone else's with a support function is refused as it was.
q "CREATE FUNCTION my_eq(a int, b int) RETURNS bool LANGUAGE plpgsql IMMUTABLE
     AS \$\$BEGIN RETURN a = b; END\$\$;
   ALTER FUNCTION my_eq(int, int) SUPPORT generate_series_int4_support;" > /dev/null
got=$(q2 "SET gp.optimizer_trace_fallback = on" "SELECT count(*) FROM pts WHERE my_eq(id, 5);")
case "$got" in
	*"extension functions with prosupport unsupported"*"1") ok "another extension's support function is refused as before" ;;
	*) notok "another extension's support function is refused as before" "$got" ;;
esac

# A function named as PostGIS's support function is not it: what is
# accepted is the code PostGIS's library runs.
q "CREATE SCHEMA lookalike;
   CREATE FUNCTION lookalike.postgis_index_supportfn(internal) RETURNS internal
     AS 'generate_series_int4_support' LANGUAGE internal;
   CREATE FUNCTION my_eq2(a int, b int) RETURNS bool LANGUAGE plpgsql IMMUTABLE
     AS \$\$BEGIN RETURN a = b; END\$\$;
   ALTER FUNCTION my_eq2(int, int) SUPPORT lookalike.postgis_index_supportfn;" > /dev/null
got=$(q2 "SET gp.optimizer_trace_fallback = on" "SELECT count(*) FROM pts WHERE my_eq2(id, 5);")
case "$got" in
	*"extension functions with prosupport unsupported"*"1") ok "and so is one that only has its name" ;;
	*) notok "and so is one that only has its name" "$got" ;;
esac

# Reading a counter is itself a statement, so what is checked is how far
# each one moved.  Over geometry: PostGIS's first geography lookup in a
# session runs two SPI queries of its own on system catalogs -- whether
# spatial_ref_sys exists, and where postgis_full_version lives -- and ORCA
# declines those, as it declines every query on a catalog.
counts="SELECT string_agg(count::text, ' ' ORDER BY reason) FROM gp_orca.fallbacks()
         WHERE reason IN ('declined', 'planned');"
read -r declined0 planned0 <<< "$(q "$counts")"
q "SELECT count(*) FROM shapes WHERE ST_Intersects(geom, $W);
   SELECT count(*) FROM zones z JOIN pts p ON ST_DWithin(p.geom, z.geom, 1);
   SELECT count(*) FROM pts WHERE ST_Contains(ST_Buffer($P, 5), geom);" > /dev/null
read -r declined1 planned1 <<< "$(q "$counts")"
[ "$declined1" = "$declined0" ] && [ "$((planned1 - planned0))" -ge 3 ] \
	&& ok "spatial queries are counted as planned, and none as declined" \
	|| notok "spatial queries are counted as planned, and none as declined" \
	         "declined $declined0 -> $declined1, planned $planned0 -> $planned1"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
