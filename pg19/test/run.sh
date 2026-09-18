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
# Run the port's test suites.
#
#   pg19/test/run.sh            every suite
#   pg19/test/run.sh load       just that one
#
# Each suite is a directory here with a run.sh in it.  A suite exits non-zero
# when anything in it failed, and so does this.
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

failed=()
skipped=()
for s in "${suites[@]}"; do
	if [ ! -x "$here/$s/run.sh" ]; then
		echo "no such suite: $s" >&2
		failed+=("$s")
		continue
	fi
	echo "=============================================================="
	echo "suite: $s"
	echo "=============================================================="
	"$here/$s/run.sh"
	rc=$?
	if [ "$rc" -eq 77 ]; then
		skipped+=("$s")
	elif [ "$rc" -ne 0 ]; then
		failed+=("$s")
	fi
	echo
done

[ "${#skipped[@]}" -gt 0 ] && echo "skipped suites: ${skipped[*]}"
if [ "${#failed[@]}" -gt 0 ]; then
	echo "failed suites: ${failed[*]}"
	exit 1
fi
echo "all suites passed"
