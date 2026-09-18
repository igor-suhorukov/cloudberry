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
# Check 6: replication between the two builds, in both directions.
#
# The replication protocol and the WAL format are unchanged, so a standby of
# one build has to follow a primary of the other.  The WAL a workload produces
# also has to hold the same kinds of record, which pg_waldump --stats reports.
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

cat > "$WORKDIR/rep-workload.sql" <<'SQL'
CREATE TABLE rep (id int primary key, g int, txt text);
INSERT INTO rep SELECT i, i % 5, repeat('r', (i % 20) + 1)
  FROM generate_series(1, 3000) i;
CREATE INDEX rep_g ON rep (g);
UPDATE rep SET g = g + 1 WHERE id % 7 = 0;
DELETE FROM rep WHERE id % 89 = 0;
CHECKPOINT;
SQL

# replicate <primary-tag> <primary-prefix> <standby-tag> <standby-prefix> <off>
replicate() {
	local ptag="$1" p="$2" stag="$3" s="$4" off="$5"
	local pdata="$WORKDIR/rep-$ptag-p" sdata="$WORKDIR/rep-$ptag-s"
	local pport sport
	pport=$(port_for "rep$ptag" "$off")
	sport=$(port_for "rep$ptag" $((off + 1)))

	pg_init "$p" "$pdata" || { notok "initdb ($ptag primary)"; return 1; }
	pg_start "$p" "$pdata" "$pport" \
		"wal_level = replica" "max_wal_senders = 4" "hot_standby = on" \
		|| { notok "$ptag primary starts" "$(tail -10 "$pdata.log")"; return 1; }

	psql_at "$p" "$pport" \
		"SELECT pg_create_physical_replication_slot('s1');" > /dev/null

	# Base backup with the standby's own tools, which is how a standby of a
	# different build would really be made.
	if ! PGHOST="$SOCKDIR" pg_run "$s" pg_basebackup -p "$pport" -D "$sdata" \
			-X stream -R -S s1 > "$sdata.basebackup.log" 2>&1; then
		notok "$stag pg_basebackup from a $ptag primary" "$(tail -10 "$sdata.basebackup.log")"
		pg_stop "$p" "$pdata"; return 1
	fi
	ok "$stag pg_basebackup from a $ptag primary"

	if ! pg_start "$s" "$sdata" "$sport"; then
		notok "$stag standby starts against a $ptag primary" "$(tail -20 "$sdata.log")"
		pg_stop "$p" "$pdata"; return 1
	fi
	ok "$stag standby starts against a $ptag primary"

	psql_file "$p" "$pport" "$WORKDIR/rep-workload.sql" > "$pdata.workload" 2>&1
	local lsn; lsn=$(psql_at "$p" "$pport" "SELECT pg_current_wal_lsn();")

	# Wait for the standby to catch up.
	local caught=no
	for _ in $(seq 1 60); do
		if [ "$(psql_at "$s" "$sport" \
				"SELECT pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn;")" = "t" ]; then
			caught=yes; break
		fi
		sleep 1
	done

	if [ "$caught" = yes ]; then
		local a b
		a=$(psql_at "$p" "$pport" "SELECT count(*), sum(id), sum(g) FROM rep;")
		b=$(psql_at "$s" "$sport" "SELECT count(*), sum(id), sum(g) FROM rep;")
		if [ "$a" = "$b" ] && [ -n "$a" ]; then
			ok "$ptag primary -> $stag standby replays the workload ($a)"
		else
			notok "$ptag primary -> $stag standby" "primary: $a / standby: $b"
		fi
	else
		notok "$stag standby caught up with the $ptag primary" \
			"$(tail -10 "$sdata.log")"
	fi

	# Promote, then check the indexes still agree with the heap.
	pg_run "$s" pg_ctl -D "$sdata" -w -t 60 promote > /dev/null 2>&1
	local amcheck; amcheck=$(PGHOST="$SOCKDIR" pg_run "$s" pg_amcheck -p "$sport" \
		-d postgres --heapallindexed --install-missing 2>&1)
	if [ $? -eq 0 ]; then
		ok "$stag standby promotes, and pg_amcheck passes"
	else
		notok "$stag promotion followed by pg_amcheck" "$amcheck"
	fi

	# The record types the workload produced.  pg_waldump reads from a
	# starting segment, so it is given the first and last the primary has.
	local segs first last
	segs=$(ls "$pdata/pg_wal" 2>/dev/null | grep -E '^[0-9A-F]{24}$' | sort)
	first=$(echo "$segs" | head -1)
	last=$(echo "$segs" | tail -1)
	if [ -n "$first" ]; then
		pg_run "$p" pg_waldump --stats -p "$pdata/pg_wal" "$first" "$last" \
			2> /dev/null \
			| sed -n 's/^\([A-Za-z0-9_/]*\) *[0-9].*/\1/p' \
			| sort -u > "$WORKDIR/wal-$ptag.txt"
	fi

	pg_stop "$s" "$sdata"
	pg_stop "$p" "$pdata"
}

replicate vanilla "$PG_VANILLA" patched "$PG_PATCHED" 0
replicate patched "$PG_PATCHED" vanilla "$PG_VANILLA" 4

if [ -s "$WORKDIR/wal-vanilla.txt" ] && [ -s "$WORKDIR/wal-patched.txt" ]; then
	compare "the same workload writes the same WAL record types" \
		"$WORKDIR/wal-vanilla.txt" "$WORKDIR/wal-patched.txt"
else
	skip "pg_waldump --stats record types" "no output from pg_waldump"
fi
