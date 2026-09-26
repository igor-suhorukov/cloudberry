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
# M7: PostGIS in a cluster -- stock PostGIS, the image's, on a coordinator and
# three segments, beside one node that loads none of the port's modules,
# which is PostgreSQL 19 as PGDG's PostGIS knows it: what the cluster answers
# is checked against what that node answers, on the same data.  The tests the
# plan lists ("PostGIS on the hook-based Cloudberry", "Tests"), but for the
# first, PostGIS's own regression suite, which runs on one node
# (postgis_regress):
#
#   1. the extensions made on a cluster, their tables replicated (decision
#      14b), their scripts run by each node, the same PostGIS on every node
#      and its raster settings on the segments;
#   2. ST_Transform in the segments' part of a plan, with an SRID the
#      session inserted;
#   3. ANALYZE's statistics, PostGIS's two kinds of its own among them, on
#      the coordinator for a distributed table and every partition;
#   4. KNN, a spatial join, ST_Union, ST_MakeLine ... ORDER BY and ST_AsMVT
#      as one node answers, ST_Union in one stage (decision 14a);
#   5. AddGeometryColumn, postgis_extensions_upgrade() and an EXCLUDE
#      constraint, which a table hashed on another column refuses;
#   6. topology on the coordinator (decision 14c);
#   7. a PostGIS database dumped and restored, and the coordinator restarted
#      without the port's modules.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/postgis_cluster/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# PostGIS is built into the image the Compose project makes.
if [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/postgis.control" ] ||
   [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/postgis_topology.control" ]; then
	echo "PostGIS is not installed; skipping"
	exit 77
fi

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-postgis-cluster-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbpc-XXXXXX)"
BASEPORT="${PGPORT:-$((6900 + RANDOM % 200))}"
PRELOAD='gp_core,gp_orca,gp_sql'
SECRET="postgis-cluster-$RANDOM$RANDOM$RANDOM"
# ORCA's note of a column with no statistics is not what is compared
export PGOPTIONS="-c gp.optimizer_print_missing_stats=off"

pass=0; fail=0
ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | head -12 | sed 's/^/         /'
         fail=$((fail + 1)); }

# node 0 is the coordinator, 1..3 the segments; node 9 is the one node
datadir() { echo "$ROOT/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }
NODES="0 1 2 3"

cleanup() {
	for n in $NODES 9; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/postgis-cluster-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
	rm -rf "$SOCK"
}
trap cleanup EXIT

q() {						# q <node> <db> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d "$2" -c "$3" 2>&1
}
qf() {						# qf <node> <db>: statements on stdin, one session
	"$PSQL" -X -q -t -A -h "$(sockdir "$1")" -p "$(port "$1")" -d "$2" -f - 2>&1
}
is() {						# is <name> <node> <db> <sql> <want>
	local got; got=$(q "$2" "$3" "$4")
	[ "$got" = "$5" ] && ok "$1" || notok "$1" "want [$5], got [$got]"
}
# alike <name> <sql>: the cluster's answer is the one node's
alike() {
	local c o; c=$(q 0 geo "$2"); o=$(q 9 geo "$2")
	[ "$c" = "$o" ] && [ -n "$c" ] && ok "$1" || notok "$1" "cluster [$c], one node [$o]"
}
# geom_alike <name> <sql of one geometry>: ST_Equals of the two answers
geom_alike() {
	local c o; c=$(q 0 geo "SELECT encode(ST_AsEWKB(($2)), 'hex')")
	o=$(q 9 geo "SELECT ST_Equals(ST_GeomFromEWKB(decode('$c', 'hex')), ($2))")
	[ "$o" = t ] && ok "$1" || notok "$1" "cluster [$c], ST_Equals [$o]"
}

start_node() {				# start_node <n> [extra lines for postgresql.auto.conf]
	local n="$1"; shift
	"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "node $n did not start"; tail -20 "$ROOT/node$n.log"; exit 1; }
}

echo "M7 PostGIS in a cluster"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo

CONF="$ROOT/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
done > "$CONF"
for n in $NODES 9; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 > "$ROOT/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
	{
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "fsync = off"
		if [ "$n" != 9 ]; then
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			echo "max_prepared_transactions = 16"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		fi
	} >> "$(datadir "$n")/postgresql.auto.conf"
done
for n in 1 2 3 0 9; do start_node "$n"; done
q 0 postgres "CREATE DATABASE geo" > /dev/null
q 9 postgres "CREATE DATABASE geo" > /dev/null
for m in gp_core gp_sql gp_orca; do q 0 geo "CREATE EXTENSION $m" > /dev/null; done

###############################################################################
echo "1. the extensions, made on the cluster"
###############################################################################
for e in postgis postgis_raster postgis_topology; do
	out=$(q 0 geo "SET client_min_messages = error; CREATE EXTENSION $e")
	[ -z "$out" ] && ok "CREATE EXTENSION $e on the coordinator" || notok "CREATE EXTENSION $e" "$out"
	q 9 geo "CREATE EXTENSION $e" > /dev/null
done
is "spatial_ref_sys is replicated, as a script's table is (decision 14b)" 0 geo \
   "SELECT policytype FROM gp_distribution_policy WHERE localoid = 'spatial_ref_sys'::regclass" "r"
is "and every segment has every row the script inserted, each running the script" 0 geo \
   "SELECT count(DISTINCT gp_segment_id), count(*) / 3 = (SELECT count(*) FROM spatial_ref_sys)
      FROM gp_dist_random('spatial_ref_sys')" "3|t"
file=$(q 0 geo "SELECT pg_relation_filepath('spatial_ref_sys')")
size=$(stat -c %s "$(datadir 0)/$file" 2>&1)
[ "$size" = 0 ] && ok "the coordinator keeps none of its rows, as of any distributed table" \
	|| notok "the coordinator's copy of spatial_ref_sys" "$size"
is "topology's tables are replicated too, their foreign keys made as Cloudberry makes one" 0 geo \
   "SELECT string_agg(c.relname || ' ' || p.policytype::text, ', ' ORDER BY c.relname)
      FROM gp_distribution_policy p JOIN pg_class c ON c.oid = p.localoid
     WHERE c.relnamespace = 'topology'::regnamespace" "layer r, topology r"
out=$(q 1 geo "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'topology.layer'::regclass AND tgisinternal")
[ "$out" = 0 ] && ok "a segment running the script makes them so too, without a foreign key's triggers" \
	|| notok "a segment's foreign key" "$out"
is "the same PostGIS on every node: each segment's libraries, as the version check compared them" 0 geo \
   "SELECT count(DISTINCT result) FROM gp.exec_on_segments(
      'SELECT postgis_lib_version() || postgis_geos_version() || postgis_gdal_version()')" "1"
# a segment whose extension says another version: CREATE EXTENSION IF NOT
# EXISTS, which every node passes over, is refused after it
q 1 geo "SET allow_system_table_mods = on; UPDATE pg_extension SET extversion = '0.1' WHERE extname = 'postgis'" > /dev/null
out=$(q 0 geo "SET client_min_messages = warning; CREATE EXTENSION IF NOT EXISTS postgis")
case "$out" in
	*'extension "postgis" is not the same on segment 0 as on the coordinator'*'The segment has "0.1"'*)
		ok "and a segment with another version of it is refused, and says which" ;;
	*) notok "a segment with another version" "$out" ;;
esac
q 1 geo "SET allow_system_table_mods = on; UPDATE pg_extension SET extversion = (SELECT default_version FROM pg_available_extensions WHERE name = 'postgis') WHERE extname = 'postgis'" > /dev/null
out=$(printf '%s\n' "SET postgis.gdal_enabled_drivers = 'GTiff PNG';" \
	"SET postgis.gdal_vsi_options = 'AWS_NO_SIGN_REQUEST=YES';" \
	"SELECT string_agg(DISTINCT result, ',') FROM gp.exec_on_segments(\$\$SELECT current_setting('postgis.gdal_enabled_drivers') || ' / ' || current_setting('postgis.gdal_vsi_options')\$\$);" | qf 0 geo)
[ "$out" = "GTiff PNG / AWS_NO_SIGN_REQUEST=YES" ] && ok "raster's settings reach the segments" \
	|| notok "raster's settings on the segments" "$out"

###############################################################################
echo "2. ST_Transform in the segments' part of a plan"
###############################################################################
cat > "$ROOT/data.sql" << 'EOF'
SET client_min_messages = warning;
CREATE TABLE pts (id int, geom geometry(Point, 4326));
CREATE TABLE polys (id int, geom geometry(Polygon, 4326));
INSERT INTO pts SELECT g, ST_SetSRID(ST_MakePoint((g * 37) % 360 - 180, (g * 53) % 170 - 85), 4326)
  FROM generate_series(1, 2000) g;
INSERT INTO polys SELECT g, ST_MakeEnvelope((g * 29) % 340 - 170, (g * 31) % 160 - 80,
                                            (g * 29) % 340 - 160, (g * 31) % 160 - 70, 4326)
  FROM generate_series(1, 60) g;
CREATE INDEX pts_gix ON pts USING gist (geom);
CREATE INDEX polys_gix ON polys USING gist (geom);
INSERT INTO spatial_ref_sys (srid, auth_name, auth_srid, srtext, proj4text)
  VALUES (990001, 'test', 990001, '', '+proj=longlat +datum=WGS84 +no_defs');
ANALYZE pts; ANALYZE polys;
EOF
out=$(qf 0 geo < "$ROOT/data.sql"; qf 9 geo < "$ROOT/data.sql")
[ -z "$out" ] && ok "the same points and polygons on both" || notok "the data" "$out"
is "the points are hashed over the three segments" 0 geo \
   "SELECT count(DISTINCT gp_segment_id) FROM pts" "3"
plan=$(q 0 geo "EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(ST_X(ST_Transform(geom, 3857))) FROM pts")
case "$plan" in
	*"Gather Motion"*"Partial Aggregate"*"st_transform"*"Seq Scan on public.pts"*"Optimizer: GPORCA"*)
		ok "ORCA runs ST_Transform on the segments, below the Gather" ;;
	*) notok "ST_Transform's place in the plan" "$(printf '%s' "$plan" | tr '\n' '|')" ;;
esac
alike "and it answers as one node does" \
      "SELECT count(*), round(sum(ST_X(ST_Transform(geom, 3857)))::numeric, 3) FROM pts"
alike "an SRID the session inserted is every segment's" \
      "SELECT round(sum(ST_X(ST_Transform(ST_SetSRID(geom, 990001), 3857)))::numeric, 3) FROM pts WHERE id <= 500"
out=$(printf '%s\n' "INSERT INTO spatial_ref_sys (srid, auth_name, auth_srid, srtext, proj4text) VALUES (990002, 'test', 990002, '', '+proj=longlat +datum=WGS84 +no_defs');" \
	"SELECT count(*) FROM pts WHERE ST_X(ST_Transform(ST_SetSRID(geom, 990002), 3857)) > 0;" | qf 0 geo)
want=$(q 9 geo "SELECT count(*) FROM pts WHERE ST_X(ST_Transform(geom, 3857)) > 0")
[ "$out" = "$want" ] && ok "one inserted in the same session too, as the segments' part of its transaction" \
	|| notok "an SRID of the same session" "cluster [$out], one node [$want]"

###############################################################################
echo "3. ANALYZE: PostGIS's statistics on the coordinator"
###############################################################################
kinds="(SELECT string_agg(k::text, ',' ORDER BY k) FROM unnest(ARRAY[stakind1, stakind2, stakind3, stakind4, stakind5]) k WHERE k IN (102, 103))"
is "a distributed table's geometry column has kinds 102 and 103, N-D and 2-D" 0 geo \
   "SELECT $kinds FROM pg_statistic WHERE starelid = 'pts'::regclass AND staattnum = 2" "102,103"
out=$(printf '%s\n' "SET client_min_messages = warning;" "CREATE TABLE parted (id int, geom geometry(Point, 4326)) PARTITION BY RANGE (id);" \
	"CREATE TABLE parted_1 PARTITION OF parted FOR VALUES FROM (1) TO (1001);" \
	"CREATE TABLE parted_2 PARTITION OF parted FOR VALUES FROM (1001) TO (2001);" \
	"INSERT INTO parted SELECT id, geom FROM pts;" "ANALYZE parted;" | qf 0 geo)
[ -z "$out" ] && ok "a partitioned table is made, filled and analyzed" || notok "the partitioned table" "$out"
is "and every partition's column has them, and the root's" 0 geo \
   "SELECT string_agg(c.relname || ':' || $kinds, ' ' ORDER BY c.relname)
      FROM pg_statistic s JOIN pg_class c ON c.oid = s.starelid
     WHERE c.relname IN ('parted', 'parted_1', 'parted_2') AND s.staattnum = 2" \
   "parted:102,103 parted_1:102,103 parted_2:102,103"

###############################################################################
echo "4. what one node answers, the cluster answers"
###############################################################################
alike "KNN: the ten points nearest a point" \
      "SELECT string_agg(id::text, ',') FROM (SELECT id FROM pts ORDER BY geom <-> 'SRID=4326;POINT(0 0)'::geometry, id LIMIT 10) s"
alike "a spatial join" \
      "SELECT count(*), sum(p.id + q.id) FROM pts p JOIN polys q ON ST_Intersects(p.geom, q.geom)"
alike "ST_DWithin through the index" \
      "SELECT count(*) FROM pts WHERE ST_DWithin(geom, 'SRID=4326;POINT(10 10)'::geometry, 20)"
geom_alike "ST_Union" "SELECT ST_Union(geom) FROM polys WHERE id <= 20"
geom_alike "ST_MakeLine ... ORDER BY" "SELECT ST_MakeLine(geom ORDER BY id) FROM pts WHERE id <= 300"
alike "ST_AsMVT" \
      "SELECT md5(ST_AsMVT(q, 'layer', 4096, 'geom', 'id' ORDER BY q.id)) FROM
         (SELECT id, ST_AsMVTGeom(ST_Transform(geom, 3857), ST_TileEnvelope(1, 1, 0)) AS geom
            FROM pts WHERE id <= 1000) q WHERE q.geom IS NOT NULL"
alike "ST_Extent, which runs in two stages" "SELECT ST_Extent(geom)::text FROM pts"
is "ST_Union has no combine function, serial functions or parallel safety here (decision 14a)" 0 geo \
   "SELECT count(*) FILTER (WHERE aggcombinefn = 0 AND aggserialfn = 0 AND aggdeserialfn = 0),
           count(*) FILTER (WHERE p.proparallel = 'u')
      FROM pg_aggregate a JOIN pg_proc p ON p.oid = a.aggfnoid
     WHERE p.proname = 'st_union' AND p.proargtypes[0] = 'geometry'::regtype" "2|2"
is "where PostgreSQL alone keeps them" 9 geo \
   "SELECT count(*) FROM pg_aggregate a JOIN pg_proc p ON p.oid = a.aggfnoid
     WHERE p.proname = 'st_union' AND aggcombinefn <> 0" "2"
plan=$(q 0 geo "EXPLAIN (COSTS OFF) SELECT ST_Union(geom) FROM polys")
case "$plan" in
	*"Partial"*) notok "no plan splits ST_Union" "$(printf '%s' "$plan" | tr '\n' '|')" ;;
	*"Aggregate"*"Gather Motion"*) ok "no plan splits ST_Union: one Aggregate, over the Gather" ;;
	*) notok "ST_Union's plan" "$(printf '%s' "$plan" | tr '\n' '|')" ;;
esac

###############################################################################
echo "5. DDL from functions, and an EXCLUDE constraint"
###############################################################################
out=$(q 0 geo "CREATE TABLE agc (id int) DISTRIBUTED BY (id); SELECT AddGeometryColumn('public', 'agc', 'g', 4326, 'POINT', 2)")
case "$out" in
	*"public.agc.g SRID:4326 TYPE:POINT DIMS:2"*) ok "AddGeometryColumn, whose ALTER TABLE a function runs" ;;
	*) notok "AddGeometryColumn" "$out" ;;
esac
is "reaches every segment" 0 geo \
   "SELECT string_agg(DISTINCT result, ',') FROM gp.exec_on_segments(
      \$\$SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'agc'::regclass AND attname = 'g'\$\$)" \
   "geometry(Point,4326)"
out=$(q 0 geo "SET client_min_messages = warning; SELECT postgis_extensions_upgrade()")
case "$out" in
	*ERROR*) notok "postgis_extensions_upgrade()" "$out" ;;
	*) ok "postgis_extensions_upgrade(), whose ALTER EXTENSION a function runs" ;;
esac
is "and leaves every node's version the coordinator's" 0 geo \
   "SELECT count(*) FROM gp.exec_on_segments(\$\$SELECT extversion FROM pg_extension WHERE extname = 'postgis'\$\$)
     WHERE result = (SELECT extversion FROM pg_extension WHERE extname = 'postgis')" "3"
is "ST_Union one stage still, as the update scripts made it again" 0 geo \
   "SELECT count(*) FILTER (WHERE aggcombinefn <> 0), count(*) FROM pg_aggregate a JOIN pg_proc p ON p.oid = a.aggfnoid
     WHERE p.proname = 'st_union' AND p.proargtypes[0] = 'geometry'::regtype" "0|2"
out=$(q 0 geo "CREATE TABLE excl (id int, g geometry, EXCLUDE USING gist (g WITH &&)) DISTRIBUTED BY (id)")
case "$out" in
	*"exclusion constraint is not compatible with the table's distribution policy"*)
		ok "an EXCLUDE constraint is refused on a table hashed on another column, as Cloudberry refuses it" ;;
	*) notok "EXCLUDE on a hashed table" "$out" ;;
esac
out=$(q 0 geo "CREATE TABLE exclr (id int, g geometry, EXCLUDE USING gist (g WITH &&)) DISTRIBUTED REPLICATED;
               INSERT INTO exclr VALUES (1, 'POINT(0 0)'); INSERT INTO exclr VALUES (2, 'POINT(0 0)')")
case "$out" in
	*"conflicting key value violates exclusion constraint"*) ok "and enforced on a replicated one" ;;
	*) notok "EXCLUDE on a replicated table" "$out" ;;
esac

###############################################################################
echo "6. topology, on the coordinator (decision 14c)"
###############################################################################
is "every function of postgis_topology is labelled to run on the coordinator" 0 geo \
   "SELECT count(*) = count(s.label) AND count(*) > 100
      FROM pg_depend d LEFT JOIN pg_seclabel s ON s.objoid = d.objid AND s.classoid = d.classid
       AND s.provider = 'gp' AND s.label LIKE '%execute_on=coordinator%'
     WHERE d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'postgis_topology')
       AND d.deptype = 'e' AND d.classid = 'pg_proc'::regclass" "t"
is "and the segments have the labels too" 1 geo \
   "SELECT count(*) > 100 FROM pg_seclabel WHERE label LIKE '%execute_on=coordinator%'" "t"
is "so ORCA leaves a query that calls one to the planner" 0 geo \
   "EXPLAIN (COSTS OFF) SELECT topology.GetTopologyID('topo1')" "Result
Optimizer: Postgres query optimizer"
out=$(printf '%s\n' "SET client_min_messages = error;" \
	"SELECT topology.CreateTopology('topo1', 4326) > 0;" \
	"SELECT topology.TopoGeo_AddLineString('topo1', 'SRID=4326;LINESTRING(0 0, 1 1)'::geometry);" \
	"SELECT count(*) FROM topology.TopoGeo_AddLineString('topo1', 'SRID=4326;LINESTRING(0 1, 1 0)'::geometry);" \
	"SELECT count(*) FROM topo1.edge_data;" "SELECT count(*) FROM topo1.node;" \
	"SELECT count(*) FROM topology.ValidateTopology('topo1');" | qf 0 geo | tr '\n' ' ')
[ "$out" = "t 1 2 4 5 0 " ] && ok "a topology made, edited and validated through the coordinator" \
	|| notok "topology" "$out"
is "its tables are distributed tables" 0 geo \
   "SELECT count(*) FROM gp_distribution_policy WHERE localoid::regclass::text LIKE 'topo1.%'" "4"
is "and it is dropped" 0 geo \
   "SET client_min_messages = warning; SELECT topology.DropTopology('topo1')" "Topology 'topo1' dropped"

###############################################################################
echo "7. a PostGIS database dumped, and the coordinator without the modules"
###############################################################################
# into a database with nothing in it: the dump makes the extensions
q 0 postgres "CREATE DATABASE geo_copy" > /dev/null
"$BINDIR/pg_dump" -h "$(sockdir 0)" -p "$(port 0)" -d geo > "$ROOT/geo.sql" 2> "$ROOT/geo.err"
"$PSQL" -X -q -h "$(sockdir 0)" -p "$(port 0)" -d geo_copy -f "$ROOT/geo.sql" > "$ROOT/geo_copy.out" 2>&1
out=$(grep -E 'ERROR|FATAL' "$ROOT/geo_copy.out" "$ROOT/geo.err")
[ -z "$out" ] && ok "pg_dump of the database, restored into another" || notok "the PostGIS dump" "$out"
is "a user's SRID, which PostGIS's dump carries, is on every segment" 0 geo_copy \
   "SELECT count(*) FROM gp_dist_random('spatial_ref_sys') WHERE srid IN (990001, 990002)" "6"
is "and ST_Transform with it answers on the segments as it did" 0 geo_copy \
   "SELECT round(sum(ST_X(ST_Transform(ST_SetSRID(geom, 990001), 3857)))::numeric, 3) FROM pts WHERE id <= 500" \
   "$(q 0 geo "SELECT round(sum(ST_X(ST_Transform(ST_SetSRID(geom, 990001), 3857)))::numeric, 3) FROM pts WHERE id <= 500")"
is "the points hashed as they were" 0 geo_copy \
   "SELECT gp_segment_id, count(*) FROM pts GROUP BY 1 ORDER BY 1" \
   "$(q 0 geo "SELECT gp_segment_id, count(*) FROM pts GROUP BY 1 ORDER BY 1")"

"$BINDIR/pg_ctl" -D "$(datadir 0)" -m fast stop > /dev/null 2>&1
"$BINDIR/pg_ctl" -D "$(datadir 0)" -l "$ROOT/node0-bare.log" -o "-c shared_preload_libraries=" -w -t 60 start > /dev/null 2>&1 \
	|| { notok "the coordinator starts without the port's modules" "$(tail -5 "$ROOT/node0-bare.log")"; }
geom_alike "restarted without the port's modules, the coordinator's PostGIS answers as PostgreSQL's" \
           "SELECT ST_Transform('SRID=4326;POINT(10 20)'::geometry, 3857)"
is "and a spatial function over constants too" 0 geo \
   "SELECT ST_Area(ST_Buffer('POINT(0 0)'::geometry, 1, 64))::numeric(10,6)" \
   "$(q 9 geo "SELECT ST_Area(ST_Buffer('POINT(0 0)'::geometry, 1, 64))::numeric(10,6)")"
"$BINDIR/pg_ctl" -D "$(datadir 0)" -m fast stop > /dev/null 2>&1
start_node 0

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
