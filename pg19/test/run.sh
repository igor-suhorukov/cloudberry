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
# Run the port's test suites, as jobs side by side.
#
#   pg19/test/run.sh                 every suite
#   pg19/test/run.sh load cluster    just those
#   JOBS=1 pg19/test/run.sh          one job at a time
#   PASSES=planner pg19/test/run.sh  the planner pass of the two-pass suites
#
# Each suite is a directory here with a run.sh in it, which exits non-zero
# when anything in it failed, 77 when it has nothing to run against.  A suite
# is one job, or the jobs "jobs" lists for it -- a suite that runs its tests
# under the planner and under ORCA is one job a pass -- and "jobs" says how
# long each takes, so that the longest start first.  Every suite makes its
# own servers, in directories of its own, listening on sockets in them only,
# so the jobs cannot meet.  JOBS of them run at once (all of them, when it
# is not set); each job's output is held and printed whole as it finishes,
# with the time it took, and a summary of the jobs ends the run.
#
# RESULTS_DIR, when set, is given to each job as a directory of its own
# under it, named for the job: isolation2.orca, cluster.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$#" -gt 0 ]; then
	suites=("$@")
else
	suites=()
	for d in "$here"/*/; do
		[ -x "$d/run.sh" ] && suites+=("$(basename "$d")")
	done
fi

# The jobs: a suite, the job's name ("" when the suite is one job), how long
# it takes, and what it is run with.  PASSES in the environment keeps the
# jobs of those passes only.
j_suite=(); j_name=(); j_secs=(); j_env=()
failed=()
for s in "${suites[@]}"; do
	if [ ! -x "$here/$s/run.sh" ]; then
		echo "no such suite: $s" >&2
		failed+=("$s")
		continue
	fi
	listed=0
	while read -r suite name secs env; do
		listed=1
		[ "$name" = "-" ] && name=""
		if [ -n "${PASSES:-}" ] && [[ " $env " =~ \ PASSES=([^ ]+)\  ]] &&
		   [[ " $PASSES " != *" ${BASH_REMATCH[1]} "* ]]; then
			continue
		fi
		j_suite+=("$s"); j_name+=("$name"); j_secs+=("$secs"); j_env+=("$env")
	done < <(awk -v s="$s" '$1 == s' "$here/jobs" 2> /dev/null)
	if [ "$listed" -eq 0 ]; then
		j_suite+=("$s"); j_name+=(""); j_secs+=(0); j_env+=("")
	fi
done

# Longest first; the order "jobs" gives them in among equals.
order=()
while read -r _ i; do
	order+=("$i")
done < <(for i in "${!j_suite[@]}"; do echo "${j_secs[$i]} $i"; done | sort -s -k1,1nr)

label() {
	echo "${j_suite[$1]}${j_name[$1]:+ (${j_name[$1]})}"
}

LOGS="$(mktemp -d "${TMPDIR:-/tmp}/cb-run-XXXXXX")"
declare -A running=()			# pid -> job
cleanup() {
	[ "${#running[@]}" -gt 0 ] && kill "${!running[@]}" 2> /dev/null
	wait 2> /dev/null
	rm -rf "$LOGS"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

max="${JOBS:-${#order[@]}}"
[ "$max" -ge 1 ] 2> /dev/null || max=1

start_job() {
	local i="$1" results=""

	if [ -n "${RESULTS_DIR:-}" ]; then
		results="$RESULTS_DIR/${j_suite[$i]}${j_name[$i]:+.${j_name[$i]}}"
		mkdir -p "$results"
	fi
	# shellcheck disable=SC2086 -- the job's settings are words
	( [ -n "$results" ] && export RESULTS_DIR="$results"
	  exec env ${j_env[$i]} "$here/${j_suite[$i]}/run.sh" ) \
		< /dev/null > "$LOGS/$i.log" 2>&1 &
	running[$!]="$i"
	j_start[i]="$(date +%s%N)"
}

j_start=(); j_ms=(); j_rc=()
t0="$(date +%s%N)"
skipped=()
next=0
while [ "$next" -lt "${#order[@]}" ] || [ "${#running[@]}" -gt 0 ]; do
	while [ "$next" -lt "${#order[@]}" ] && [ "${#running[@]}" -lt "$max" ]; do
		start_job "${order[$next]}"
		next=$((next + 1))
	done

	wait -n -p done_pid
	rc=$?
	[ -n "${done_pid:-}" ] && [ -n "${running[$done_pid]:-}" ] || continue
	i="${running[$done_pid]}"
	unset "running[$done_pid]"
	j_ms[i]=$(( ($(date +%s%N) - j_start[i]) / 1000000 ))
	j_rc[i]="$rc"

	case "$rc" in
		0)  result="passed" ;;
		77) result="skipped"; skipped+=("$(label "$i")") ;;
		*)  result="FAILED"; failed+=("$(label "$i")") ;;
	esac
	echo "=============================================================="
	printf 'suite: %s -- %s in %d s\n' "$(label "$i")" "$result" $((j_ms[i] / 1000))
	echo "=============================================================="
	cat "$LOGS/$i.log"
	echo
done

wall=$(( ($(date +%s%N) - t0) / 1000000 ))
echo "=============================================================="
printf '%d jobs, %d at most at once, in %d s:\n' "${#order[@]}" "$max" $((wall / 1000))
for i in "${order[@]}"; do
	[ -n "${j_ms[$i]:-}" ] && echo "${j_ms[$i]} $i"
done | sort -k1,1nr | while read -r ms i; do
	case "${j_rc[$i]}" in 0) r="passed" ;; 77) r="skipped" ;; *) r="FAILED" ;; esac
	printf '  %-32s %5d s  %s\n' "$(label "$i")" $((ms / 1000)) "$r"
done
echo

[ "${#skipped[@]}" -gt 0 ] && echo "skipped: ${skipped[*]}"
if [ "${#failed[@]}" -gt 0 ]; then
	echo "failed: ${failed[*]}"
	exit 1
fi
echo "all suites passed"
