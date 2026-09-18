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
# Check 7: dumps and upgrades.
#
# pg_dumpall of the same cluster has to read the same on both builds, and
# pg_upgrade has to consider a cluster of one build an acceptable source for
# the other -- which is what a user moving between them would do.
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

vport=$(port_for upgrade 0)
pport=$(port_for upgrade 1)
vdata="$WORKDIR/up-vanilla"
pdata="$WORKDIR/up-patched"

cat > "$WORKDIR/up-workload.sql" <<'SQL'
CREATE TABLE up (id int primary key, g int, txt text, d date DEFAULT current_date);
INSERT INTO up (id, g, txt) SELECT i, i % 9, 'u' || i FROM generate_series(1, 500) i;
CREATE INDEX up_g ON up (g);
CREATE VIEW up_v AS SELECT id, g FROM up WHERE g > 2;
CREATE MATERIALIZED VIEW up_m AS SELECT g, count(*) c FROM up GROUP BY g;
CREATE FUNCTION up_f(int) RETURNS int LANGUAGE sql AS $$ SELECT $1 * 2 $$;
CREATE TYPE up_t AS (a int, b text);
CREATE ROLE up_role NOLOGIN;
GRANT SELECT ON up TO up_role;
COMMENT ON TABLE up IS 'a table';
SQL

for pair in "vanilla:$PG_VANILLA:$vdata:$vport" "patched:$PG_PATCHED:$pdata:$pport"; do
	IFS=: read -r tag prefix data port <<< "$pair"
	pg_init "$prefix" "$data" || { notok "initdb ($tag)"; exit 1; }
	pg_start "$prefix" "$data" "$port" || {
		notok "server starts ($tag)" "$(tail -10 "$data.log")"; exit 1; }
	psql_file "$prefix" "$port" "$WORKDIR/up-workload.sql" > "$data.workload" 2>&1
done

###############################################################################
# The same cluster, dumped by both builds' pg_dumpall
###############################################################################
# Dump the vanilla cluster with each build's tool; the output has to match.
# Each dump carries a fresh random token on its \restrict and \unrestrict
# lines, so those are normalised before the dumps are compared.
dumpall() {								# dumpall <prefix> <port> <out>
	PGHOST="$SOCKDIR" pg_run "$1" pg_dumpall -p "$2" 2>&1 \
		| sed -E 's/^\\(un)?restrict [A-Za-z0-9]+$/\\\1restrict <token>/' > "$3"
}

dumpall "$PG_VANILLA" "$vport" "$WORKDIR/dump-by-v.sql"
dumpall "$PG_PATCHED" "$vport" "$WORKDIR/dump-by-p.sql"
compare "both builds' pg_dumpall read one cluster the same way" \
	"$WORKDIR/dump-by-v.sql" "$WORKDIR/dump-by-p.sql"

# And the two clusters, each dumped by its own build: the same workload has to
# produce the same dump.
dumpall "$PG_PATCHED" "$pport" "$WORKDIR/dump-p.sql"
compare "the same workload dumps identically from both builds" \
	"$WORKDIR/dump-by-v.sql" "$WORKDIR/dump-p.sql"

pg_stop "$PG_VANILLA" "$vdata"
pg_stop "$PG_PATCHED" "$pdata"

###############################################################################
# pg_upgrade --check, both directions
###############################################################################
upgrade_check() {						# upgrade_check <old-tag> <old> <olddata> <new-tag> <new>
	local otag="$1" old="$2" olddata="$3" ntag="$4" new="$5"
	local newdata="$WORKDIR/up-target-$otag-$ntag"
	local out="$WORKDIR/upgrade-$otag-$ntag.log"

	pg_init "$new" "$newdata" || { notok "initdb for the $ntag target"; return 1; }

	# pg_upgrade starts both servers itself and puts their sockets in
	# --socketdir, which defaults to the current directory; that has to be the
	# short one, or the socket path runs past its 107-byte limit.
	( cd "$SOCKDIR" && env "$(pg_env "$new")" "$new/bin/pg_upgrade" \
		--old-bindir "$old/bin" --new-bindir "$new/bin" \
		--old-datadir "$olddata" --new-datadir "$newdata" \
		--socketdir "$SOCKDIR" \
		--check > "$out" 2>&1 )
	if [ $? -eq 0 ]; then
		ok "pg_upgrade --check accepts a $otag cluster as a $ntag source"
	else
		notok "pg_upgrade --check ($otag -> $ntag)" "$(tail -20 "$out")"
	fi
}

upgrade_check vanilla "$PG_VANILLA" "$vdata" patched "$PG_PATCHED"
upgrade_check patched "$PG_PATCHED" "$pdata" vanilla "$PG_VANILLA"
