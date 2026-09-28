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
# PAX's unit tests: Cloudberry's googletest programs of contrib/pax_storage.
#
# Cloudberry links them into one program, test_main, with its backend built
# as a library, libpostgres.so, which PostgreSQL 19 does not make.  The port
# builds them into a module, pax_gtest, with PAX compiled in, and runs them
# in a backend, where every function of the server's they call is: a
# single-user one, a node's of its own with gp_core and gp_ao loaded, which
# PAX's code calls, and not pax, whose code the module has itself.  The
# module's function pax_gtest_run() runs the tests a googletest filter
# names, googletest printing to the backend's standard output
# (pg19/pax/src/pax_gtest.cc).  Each suite of tests runs in a backend of its
# own, from a directory of its own, where the tests write their files: a
# test that crashes the backend ends its suite's run alone.  manifest names
# a test that does not run here, and why.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -f "$("$BINDIR/pg_config" --pkglibdir)/pax_gtest.so" ]; then
	echo "pax_gtest, PAX's unit tests, is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-pax-gtest-XXXXXX")"
trap '[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"' EXIT

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }

# run <googletest filter, or flag> <output>
run() {
	mkdir -p "$WORK/cwd/$(basename "$2")"
	(cd "$WORK/cwd/$(basename "$2")" &&
	 timeout 900 "$BINDIR/postgres" --single -D "$WORK/data" \
		-c shared_preload_libraries=gp_core,gp_ao postgres > "$2" 2>&1 <<SQL
CREATE FUNCTION pax_gtest_run(text) RETURNS int AS 'pax_gtest', 'pax_gtest_run' LANGUAGE C;
SELECT pax_gtest_run('$1');
SQL
	)
	echo "exit $?" >> "$2"
}

# The tests the module has, suite by suite, less the manifest's
run --gtest_list_tests "$WORK/list"
awk '/^[A-Za-z]/ && !/LOG:|PostgreSQL|backend>|^exit/ { suite = $1 }
	 /^  [A-Za-z]/ { print suite $1 }' "$WORK/list" > "$WORK/tests"
skipped=$(awk '$1 == "skip" { print $2 }' "$HERE/manifest" 2> /dev/null | paste -sd: -)
total=$(grep -cvxF -f <(awk '$1 == "skip" { print $2 }' "$HERE/manifest" 2> /dev/null; echo) "$WORK/tests")
echo "pax_gtest: PAX's unit tests, in a single-user backend"
printf '  %d tests in %d suites, %d skipped\n' "$total" \
	"$(sed 's/\.[^.]*$//' "$WORK/tests" | sort -u | wc -l)" \
	"$(awk '$1 == "skip"' "$HERE/manifest" 2> /dev/null | wc -l)"
echo
if [ "$(wc -l < "$WORK/tests")" -eq 0 ]; then
	echo "  the module listed no tests:"
	tail -20 "$WORK/list" | sed 's/^/    /'
	exit 1
fi

passed=0
failed=()
for suite in $(sed 's/\.[^.]*$//' "$WORK/tests" | awk '!seen[$0]++'); do
	out="$WORK/$(echo "$suite" | tr / _).out"
	run "$suite.*${skipped:+-$skipped}" "$out"
	ok=$(grep -c '^\[       OK \]' "$out")
	bad=$(grep -c '^\[  FAILED  \] [A-Za-z].*(' "$out")
	n=$(grep -c "^$suite\." "$WORK/tests")
	n=$((n - $(awk -v s="$suite." '$1 == "skip" && index($2, s) == 1' "$HERE/manifest" 2> /dev/null | wc -l)))
	passed=$((passed + ok))
	if [ "$ok" -eq "$n" ] && [ "$(tail -1 "$out")" = "exit 0" ]; then
		printf '  ok     %-60s %4d\n' "$suite" "$ok"
	else
		printf '  not ok %-60s %4d of %d, %s\n' "$suite" "$ok" "$n" "$(tail -1 "$out")"
		failed+=("$suite")
		grep '^\[  FAILED  \] [A-Za-z].*(' "$out" | sed 's/^/           /'
		grep -m1 -A3 'TRAP:\|terminate called\|Segmentation' "$out" | sed 's/^/           /'
		[ -n "${RESULTS_DIR:-}" ] && cp "$out" "$RESULTS_DIR/"
	fi
done
echo "  $passed of $total passed"
[ "${#failed[@]}" -eq 0 ]
