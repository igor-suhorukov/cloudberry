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
# Check 9: performance, in instructions.
#
# Rule 6 asks that an insertion point reached per row, per page or per
# statement cost nothing measurable while no module uses it.  This counts
# the instructions each workload takes on either build, with valgrind's
# cachegrind, rather than timing it, which a shared machine makes noise of.
# A workload runs in a single-user backend (postgres --single): one process,
# with no background work to vary, on a copy of a data directory each
# build's initdb made and loaded alike, its statements read from a file.  A
# workload's count is the backend's, less that of one that only starts and
# stops; each runs twice on each build, and the spread of the two is the
# noise.
#
# The builds are the ones without assertions, where the image has them
# (PG_VANILLA_NOASSERT, PG_PATCHED_NOASSERT): no production server runs an
# assertion's instructions, and they would thin out a hook's share.  Where
# it has not, the builds the other checks run, and says so.  A workload
# passes if the patched build's count is within CHECK9_THRESHOLD percent of
# the vanilla build's, 0.5 by default: a threshold proposed with this check
# (cloudberry.md, "Checking that the server stays vanilla"), which the
# numbers it prints are for deciding.
#
# The workloads, and the insertion points they reach unused:
#   select    pgbench -S's point query, 20,000 statements: each one's
#             parse (O26), its lock (O30), plan and execution
#   tpcb      pgbench -N's transaction, 2,000 of them: UPDATE, SELECT and
#             INSERT, each row updated asking O20, the history growing (O21)
#   update    an UPDATE of a unique column, which no HOT update is, over
#             50,000 rows, and UPDATE ... RETURNING, DELETE ... RETURNING and
#             MERGE: O16 for each row and unique index, O20 for each row
#   combocid  a transaction that updates its own rows again and reads them:
#             a combo command ID made and looked up for each (R2)
#   copy      COPY FROM a file of 200,000 rows, CREATE INDEX and DROP TABLE:
#             each file extension (O21), each file removed (O22), and the
#             table access method registry's tests (O13-O19)
#   plan      EXPLAIN of a six-way join, 300 times: the planner, O15's
#             physical target list, O4's node labels
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

if ! command -v valgrind > /dev/null; then
	skip "instruction counts" "valgrind is not installed in this image"
	exit 0
fi

vanilla="$PG_VANILLA"
patched="$PG_PATCHED"
builds="the builds with assertions"
if [ -x "${PG_VANILLA_NOASSERT:-/nonexistent}/bin/postgres" ] &&
   [ -x "${PG_PATCHED_NOASSERT:-/nonexistent}/bin/postgres" ]; then
	vanilla="$PG_VANILLA_NOASSERT"
	patched="$PG_PATCHED_NOASSERT"
	builds="the builds without assertions"
fi
threshold="${CHECK9_THRESHOLD:-0.5}"
PERF="$WORKDIR/perf"
mkdir -p "$PERF"
echo "  $builds: $vanilla, $patched; threshold $threshold%"

###############################################################################
# The data, loaded alike by each build
###############################################################################
python3 - "$PERF" <<'PY'
import sys
d = sys.argv[1]

def write(name, stmts):
    # postgres --single -j: a statement ends at a semicolon and a blank line
    with open("%s/%s.sql" % (d, name), "w") as f:
        for s in stmts:
            f.write(s.rstrip(";") + ";\n\n")

write("setup", [
    "CREATE TABLE accounts (aid int PRIMARY KEY, bid int, abalance int, filler char(84))",
    "INSERT INTO accounts SELECT g, g % 10, 0, '' FROM generate_series(1, 100000) g",
    "CREATE TABLE history (tid int, bid int, aid int, delta int, mtime timestamp, filler char(22))",
    "CREATE TABLE uniq (id int PRIMARY KEY, k int UNIQUE, v int)",
    "INSERT INTO uniq SELECT g, g, 0 FROM generate_series(1, 50000) g"] +
    ["CREATE TABLE j%d (id int PRIMARY KEY, %s)" %
     (n, "a int" if n == 1 else "j%d int REFERENCES j%d, %s int" % (n - 1, n - 1, "abcdef"[n - 1]))
     for n in range(1, 7)] +
    ["INSERT INTO j%d SELECT %s FROM generate_series(1, 1000) g" %
     (n, "g, g" if n == 1 else "g, g, g") for n in range(1, 7)] +
    ["VACUUM ANALYZE", "CHECKPOINT"])
write("empty", ["SELECT 1"])
write("select", ["SELECT abalance FROM accounts WHERE aid = %d" % ((i * 7919) % 100000 + 1)
                 for i in range(20000)])
tpcb = []
for i in range(2000):
    aid = (i * 104729) % 100000 + 1
    delta = (i * 37) % 10001 - 5000
    tpcb += ["BEGIN",
             "UPDATE accounts SET abalance = abalance + %d WHERE aid = %d" % (delta, aid),
             "SELECT abalance FROM accounts WHERE aid = %d" % aid,
             "INSERT INTO history (tid, bid, aid, delta, mtime) VALUES (%d, %d, %d, %d, CURRENT_TIMESTAMP)"
             % (i % 10 + 1, aid % 10, aid, delta),
             "END"]
write("tpcb", tpcb)
write("update", ["UPDATE uniq SET k = k + 1000000",
                 "UPDATE uniq SET v = v + 1 WHERE id % 2 = 0 RETURNING id, v",
                 "DELETE FROM uniq WHERE id % 3 = 0 RETURNING *",
                 "MERGE INTO uniq u USING generate_series(1, 50000) g ON u.id = g "
                 "WHEN MATCHED THEN UPDATE SET v = u.v + 1 "
                 "WHEN NOT MATCHED THEN INSERT VALUES (g, g + 2000000, 0)"])
write("combocid", ["BEGIN",
                   "UPDATE uniq SET v = v + 1",
                   "UPDATE uniq SET v = v + 1",
                   "SELECT count(*) FROM uniq WHERE v > 0",
                   "DELETE FROM uniq WHERE id % 2 = 1",
                   "SELECT count(*) FROM uniq",
                   "ROLLBACK"])
with open("%s/big.dat" % d, "w") as f:
    for i in range(200000):
        f.write("%d\t%d\trow %d\n" % (i, (i * 7) % 1000, i))
write("copy", ["CREATE TABLE big (a int, b int, c text)",
               "COPY big FROM '%s/big.dat'" % d,
               "CREATE INDEX big_a ON big (a)",
               "SELECT count(*) FROM big WHERE b = 7",
               "DROP TABLE big"])
write("plan", ["EXPLAIN (COSTS OFF) SELECT j1.a, j6.f FROM j1 JOIN j2 ON j2.j1 = j1.id "
               "JOIN j3 ON j3.j2 = j2.id JOIN j4 ON j4.j3 = j3.id JOIN j5 ON j5.j4 = j4.id "
               "JOIN j6 ON j6.j5 = j5.id WHERE j1.a < %d AND j6.f > %d" % (100 + i, i)
               for i in range(300)])
PY

for b in vanilla patched; do
	prefix=$([ "$b" = vanilla ] && echo "$vanilla" || echo "$patched")
	pg_init "$prefix" "$PERF/$b-data" ||
		{ notok "initdb of the $b build" "$(tail -5 "$PERF/$b-data.initdb.log")"; exit 0; }
	pg_run "$prefix" postgres --single -j -D "$PERF/$b-data" postgres \
		< "$PERF/setup.sql" > "$PERF/$b-setup.out" 2>&1
	if grep -qE ' (ERROR|FATAL|PANIC): ' "$PERF/$b-setup.out"; then
		notok "the data loaded on the $b build" "$(grep -m3 -E ' (ERROR|FATAL|PANIC): ' "$PERF/$b-setup.out")"
		exit 0
	fi
done

###############################################################################
# The counts
###############################################################################
workloads="select tpcb update combocid copy plan"

# measure <build> <workload> <run>: the backend's instructions, on a copy of
# the build's data directory, into $PERF/<build>-<workload>-<run>.ir
measure() {
	local b=$1 w=$2 r=$3 prefix dir="$PERF/$1-$2-$3"

	prefix=$([ "$b" = vanilla ] && echo "$vanilla" || echo "$patched")
	cp -a "$PERF/$b-data" "$dir"
	env "$(pg_env "$prefix")" valgrind --tool=cachegrind --cache-sim=no \
		--cachegrind-out-file=/dev/null \
		"$prefix/bin/postgres" --single -j -D "$dir" postgres \
		< "$PERF/$w.sql" > "$dir.out" 2> "$dir.err"
	grep -oP 'I\s+refs:\s+\K[0-9,]+' "$dir.err" | tr -d , > "$dir.ir"
	grep -m1 -E ' (ERROR|FATAL|PANIC): ' "$dir.err" >> "$dir.ir"
	rm -rf "$dir"
}
export -f measure pg_env
export PERF vanilla patched

for b in vanilla patched; do
	for w in empty $workloads; do
		for r in 1 2; do
			echo "$b $w $r"
		done
	done
done | xargs -P "$(nproc)" -L 1 bash -c 'measure "$0" "$1" "$2"'

ir() { head -1 "$PERF/$1-$2-$3.ir"; }			# ir <build> <workload> <run>

for b in vanilla patched; do
	for r in 1 2; do
		if ! [[ "$(ir "$b" empty "$r")" =~ ^[0-9]+$ ]]; then
			notok "a backend that only starts and stops, on the $b build" \
				"$(tail -3 "$PERF/$b-empty-$r.err")"
			exit 0
		fi
	done
done

printf '           %-9s %15s %15s %8s %7s\n' workload vanilla patched patched noise
for w in $workloads; do
	bad=
	for b in vanilla patched; do
		for r in 1 2; do
			v=$(ir "$b" "$w" "$r")
			[[ "$v" =~ ^[0-9]+$ ]] && ! grep -qE ' (ERROR|FATAL|PANIC): ' "$PERF/$b-$w-$r.ir" ||
				bad="$b run $r: $(tail -1 "$PERF/$b-$w-$r.ir") $(tail -2 "$PERF/$b-$w-$r.err")"
		done
	done
	if [ -n "$bad" ]; then
		notok "the $w workload runs" "$bad"
		continue
	fi
	line=$(python3 - "$threshold" \
		"$(ir vanilla "$w" 1)" "$(ir vanilla "$w" 2)" "$(ir vanilla empty 1)" "$(ir vanilla empty 2)" \
		"$(ir patched "$w" 1)" "$(ir patched "$w" 2)" "$(ir patched empty 1)" "$(ir patched empty 2)" <<'PY'
import sys
t = float(sys.argv[1])
v1, v2, ve1, ve2, p1, p2, pe1, pe2 = map(int, sys.argv[2:])
v = [v1 - ve1, v2 - ve2]
p = [p1 - pe1, p2 - pe2]
vm, pm = sum(v) / 2, sum(p) / 2
delta = 100.0 * (pm - vm) / vm
noise = 100.0 * max(abs(v[0] - v[1]) / vm, abs(p[0] - p[1]) / pm)
print("%d %d %+.3f %.3f %s" % (vm, pm, delta, noise, "ok" if abs(delta) <= t else "notok"))
PY
	)
	read -r vm pm delta noise verdict <<< "$line"
	printf '           %-9s %15s %15s %7s%% %6s%%\n' "$w" "$vm" "$pm" "$delta" "$noise"
	if [ "$verdict" = ok ]; then
		ok "$w: the patched build within $threshold% of vanilla's instructions ($delta%)"
	else
		notok "$w: the patched build within $threshold% of vanilla's instructions" \
			"$delta%, the noise $noise%"
	fi
done
