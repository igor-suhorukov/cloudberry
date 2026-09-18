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
# Does the patched server still behave like vanilla PostgreSQL 19?
#
# This is the binding requirement of the port: changes to the PostgreSQL core
# must not alter how the server operates, or what it can do, when the
# Cloudberry extensions are not loaded.  Every check here compares two builds
# of the same commit -- one vanilla, one with the patch series -- and none of
# them loads a Cloudberry module.
#
# The checks are numbered as in cloudberry.md, "Checking that the server stays
# vanilla".  Those it does not cover are noted at the end.
#
#   PG_VANILLA=/prefix PG_PATCHED=/prefix pg19/test/vanilla/run.sh [check...]
#
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export PG_VANILLA="${PG_VANILLA:-/pg/vanilla}"
export PG_PATCHED="${PG_PATCHED:-/pg/patched}"

for p in "$PG_VANILLA" "$PG_PATCHED"; do
	if [ ! -x "$p/bin/postgres" ]; then
		# These checks need both builds side by side, which only the compare
		# image has.  Exit 77, which the suite dispatcher reads as "skipped",
		# so running the tests anywhere else does not fail over it.
		echo "  skipped: no PostgreSQL at $p"
		echo "  (set PG_VANILLA and PG_PATCHED, or use the compare image)"
		exit 77
	fi
done

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/cb-vanilla-XXXXXX")"
export WORKDIR
SOCKDIR="$(mktemp -d /tmp/cbs-XXXXXX)"
export SOCKDIR
export RESULTS="$WORKDIR/results"
: > "$RESULTS"
trap 'rm -rf "$WORKDIR" "${SOCKDIR:-}"' EXIT

echo "vanilla-equivalence checks"
echo "  vanilla  $PG_VANILLA  ($("$PG_VANILLA/bin/pg_config" --version))"
echo "  patched  $PG_PATCHED  ($("$PG_PATCHED/bin/pg_config" --version))"
echo "  workdir  $WORKDIR"
echo

if [ "$#" -gt 0 ]; then
	checks=()
	for c in "$@"; do checks+=("$here"/checks/"$c"*.sh); done
else
	checks=("$here"/checks/*.sh)
fi

for c in "${checks[@]}"; do
	[ -x "$c" ] || continue
	echo "$(basename "$c" .sh)"
	"$c"
	echo
done

pass=$(grep -c '^ok$'    "$RESULTS" || true)
fail=$(grep -c '^notok$' "$RESULTS" || true)
skipped=$(grep -c '^skip$' "$RESULTS" || true)

echo "  $pass passed, $fail failed, $skipped skipped"
echo
echo "  not covered here: check 8 (third-party binaries) needs PGDG packages"
echo "  for this PostgreSQL; check 9 (instruction counts) needs perf or"
echo "  cachegrind; check 12 belongs to M5, when the modules first write data"
echo "  of their own.  Check 11, that the hooks are called, is the hooks suite."

[ "$fail" -eq 0 ]
