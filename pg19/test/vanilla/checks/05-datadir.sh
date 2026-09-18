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
# Check 5: a data directory written by one build is read by the other.
#
# The rule says the on-disk format does not change, so a cluster initdb'd by
# vanilla must be usable by the patched build and then by vanilla again, with
# its data and its checksums intact -- and the same the other way round.
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

cat > "$WORKDIR/dd-workload.sql" <<'SQL'
CREATE TABLE dd (id int primary key, g int, txt text);
INSERT INTO dd SELECT i, i % 11, repeat('x', (i % 40) + 1)
  FROM generate_series(1, 5000) i;
CREATE INDEX dd_g ON dd (g);
CREATE INDEX dd_txt ON dd USING gin (to_tsvector('simple', txt));
UPDATE dd SET g = g + 1 WHERE id % 13 = 0;
DELETE FROM dd WHERE id % 97 = 0;
VACUUM (ANALYZE) dd;
SQL

# interchange <first-tag> <first-prefix> <second-tag> <second-prefix> <offset>
interchange() {
	local atag="$1" a="$2" btag="$3" b="$4" off="$5"
	local data="$WORKDIR/dd-$atag-$btag"
	local port; port=$(port_for "datadir$atag$btag" "$off")

	# initdb with the first build.
	if ! pg_init "$a" "$data"; then
		notok "initdb ($atag)" "$(tail -10 "$data.initdb.log")"; return 1
	fi

	# Write the data with the second build.
	if ! pg_start "$b" "$data" "$port"; then
		notok "$btag starts on a directory $atag created" "$(tail -20 "$data.log")"
		return 1
	fi
	ok "$btag starts on a directory $atag created"
	psql_file "$b" "$port" "$WORKDIR/dd-workload.sql" > "$data.workload" 2>&1
	local sum_b; sum_b=$(psql_at "$b" "$port" \
		"SELECT count(*), sum(id), sum(g), sum(length(txt)) FROM dd;")
	pg_stop "$b" "$data"

	# Read it back with the first build.
	if ! pg_start "$a" "$data" "$port"; then
		notok "$atag starts again on a directory $btag wrote" "$(tail -20 "$data.log")"
		return 1
	fi
	local sum_a; sum_a=$(psql_at "$a" "$port" \
		"SELECT count(*), sum(id), sum(g), sum(length(txt)) FROM dd;")
	if [ "$sum_a" = "$sum_b" ] && [ -n "$sum_a" ]; then
		ok "$atag reads back what $btag wrote ($sum_a)"
	else
		notok "$atag reads back what $btag wrote" "$atag: $sum_a / $btag: $sum_b"
	fi

	# The indexes have to agree with the heap across the handover.
	local amcheck; amcheck=$(PGHOST="$SOCKDIR" pg_run "$a" pg_amcheck \
		-p "$port" -d postgres --heapallindexed --install-missing 2>&1)
	if [ $? -eq 0 ]; then
		ok "pg_amcheck --heapallindexed passes ($atag on $btag's data)"
	else
		notok "pg_amcheck --heapallindexed" "$amcheck"
	fi
	pg_stop "$a" "$data"

	# Checksums, offline, with the build that did not write the pages.
	local cks; cks=$(pg_run "$b" pg_checksums --check -D "$data" 2>&1)
	if [ $? -eq 0 ]; then
		ok "pg_checksums --check passes ($btag on a cluster $atag last ran)"
	else
		notok "pg_checksums --check" "$cks"
	fi
}

interchange vanilla "$PG_VANILLA" patched "$PG_PATCHED" 0
interchange patched "$PG_PATCHED" vanilla "$PG_VANILLA" 2
