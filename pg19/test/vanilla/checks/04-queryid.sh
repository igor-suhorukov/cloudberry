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
#
# Check 4: query IDs.
#
# Query IDs are computed by functions generated from the node definitions, so a
# new node type, a new node field or a changed field order would change them --
# and with them every extension and monitoring tool that stores one.  The same
# workload therefore has to produce the same set of query IDs on both builds.
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

vport=$(port_for queryid 0)
pport=$(port_for queryid 1)
vdata="$WORKDIR/qid-vanilla"
pdata="$WORKDIR/qid-patched"

# A workload that reaches a good spread of node types.
cat > "$WORKDIR/qid-workload.sql" <<'SQL'
CREATE TABLE t (id int primary key, g int, txt text, d date, arr int[]);
INSERT INTO t SELECT i, i % 7, 'row ' || i, date '2020-01-01' + i, ARRAY[i, i+1]
  FROM generate_series(1, 200) i;
CREATE TABLE u (id int, val numeric);
INSERT INTO u SELECT i, i * 1.5 FROM generate_series(1, 100) i;
ANALYZE t, u;
SELECT count(*) FROM t WHERE g = 3;
SELECT g, count(*), avg(id) FROM t GROUP BY g HAVING count(*) > 5 ORDER BY g;
SELECT t.id, u.val FROM t JOIN u USING (id) WHERE t.g IN (1, 2) ORDER BY 1 LIMIT 10;
SELECT id, sum(g) OVER (PARTITION BY g ORDER BY id) FROM t;
WITH r AS (SELECT g, count(*) c FROM t GROUP BY g) SELECT * FROM r WHERE c > 1;
SELECT * FROM t WHERE txt LIKE 'row 1%' AND d > date '2020-02-01';
SELECT unnest(arr) FROM t WHERE id < 5;
SELECT (SELECT max(val) FROM u) + id FROM t WHERE id < 3;
UPDATE t SET g = g + 1 WHERE id % 50 = 0;
DELETE FROM t WHERE id > 190;
MERGE INTO u USING t ON u.id = t.id WHEN MATCHED THEN UPDATE SET val = val + 1;
SELECT a.id FROM t a LEFT JOIN t b ON a.id = b.g WHERE b.id IS NULL ORDER BY 1;
SELECT g FROM t UNION SELECT g FROM t ORDER BY 1;
SELECT grouping(g), g, count(*) FROM t GROUP BY GROUPING SETS ((g), ()) ORDER BY 2;
CREATE VIEW v AS SELECT id, g FROM t WHERE g > 0;
SELECT * FROM v WHERE id < 10;
SQL

run_one() {								# run_one <prefix> <datadir> <port> <out>
	local prefix="$1" data="$2" port="$3" out="$4"
	pg_init "$prefix" "$data" || return 1
	pg_start "$prefix" "$data" "$port" \
		"shared_preload_libraries = 'pg_stat_statements'" \
		"compute_query_id = on" || return 1
	psql_at "$prefix" "$port" "CREATE EXTENSION pg_stat_statements;" > /dev/null
	psql_file "$prefix" "$port" "$WORKDIR/qid-workload.sql" > "$out.workload" 2>&1
	# The IDs, not the texts: the texts are the same by construction.
	psql_at "$prefix" "$port" \
		"SELECT queryid FROM pg_stat_statements
		  WHERE queryid IS NOT NULL AND dbid = (SELECT oid FROM pg_database WHERE datname = 'postgres')
		  ORDER BY queryid;" > "$out"
	pg_stop "$prefix" "$data"
}

if ! run_one "$PG_VANILLA" "$vdata" "$vport" "$WORKDIR/qid-v.out"; then
	notok "vanilla ran the workload" "$(tail -10 "$vdata.log" 2>/dev/null)"
	exit 1
fi
if ! run_one "$PG_PATCHED" "$pdata" "$pport" "$WORKDIR/qid-p.out"; then
	notok "patched ran the workload" "$(tail -10 "$pdata.log" 2>/dev/null)"
	exit 1
fi

n=$(wc -l < "$WORKDIR/qid-v.out")
if [ "$n" -lt 15 ]; then
	notok "the workload produced query IDs (got $n)" "$(head -5 "$WORKDIR/qid-v.out.workload")"
else
	ok "the workload produced $n query IDs"
fi

compare "the same workload gives the same query IDs" \
	"$WORKDIR/qid-v.out" "$WORKDIR/qid-p.out"
