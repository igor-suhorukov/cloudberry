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
# Check 1: PostgreSQL's own tests, meson test, in one build's tree.
#
# It runs in the build stage of a PostgreSQL image, which keeps the tree it
# built (compose.yml: meson-vanilla, meson-patched), as a user of its own,
# since initdb refuses root: every suite meson knows, the TAP tests among
# them, as the image enables them.  It prints each test with its result,
# sorted, and the summary on stderr: the two builds' lists have to be the
# same, with no failure in either.
#
set -u

id tester > /dev/null 2>&1 || useradd -m tester
chown -R tester /src
su tester -c "cd /src && meson test -C build --no-rebuild \
	--num-processes ${MESON_JOBS:-$(nproc)} --print-errorlogs" > /tmp/meson.out 2>&1
rc=$?

# "  12/381 postgresql:regress / regress/regress    OK    25.31s"
sed -nE 's#^ *[0-9]+/[0-9]+ +([^ ]+) +/ +([^ ]+) +(OK|FAIL|SKIP|EXPECTEDFAIL|UNEXPECTEDPASS|TIMEOUT|ERROR) .*#\1 / \2 \3#p' \
	/tmp/meson.out | sort
{
	echo "PostgreSQL at $(cat /src/.pg_ref_commit 2>/dev/null)," \
		"test patches: $(cat /src/.pg_test_patches 2>/dev/null | tr '\n' ' ')"
	grep -E '^(Ok|Expected Fail|Fail|Unexpected Pass|Skipped|Timeout): ' /tmp/meson.out
	[ "$rc" -ne 0 ] && grep -E ' (FAIL|ERROR|TIMEOUT) ' /tmp/meson.out | head -50
} >&2
exit $rc
