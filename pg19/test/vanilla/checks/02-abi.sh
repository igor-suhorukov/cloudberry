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
# Check 2: the binary interface.
#
# Extensions built against vanilla PostgreSQL 19 have to keep loading into the
# patched server, so the patch series may add symbols but must not change or
# remove any.  abidiff reports that as a compatible change; an incompatible one
# sets bit 3 of its exit status (ABIDIFF_ABI_INCOMPATIBLE_CHANGE).
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

if ! command -v abidiff > /dev/null; then
	skip "abidiff" "libabigail is not installed in this image"
	exit 0
fi

out="$WORKDIR/abidiff.txt"
abidiff --no-added-syms \
	"$PG_VANILLA/bin/postgres" "$PG_PATCHED/bin/postgres" > "$out" 2>&1
rc=$?

# 0 no change; bit 2 (4) ABI change; bit 3 (8) incompatible change.
if [ $((rc & 8)) -ne 0 ]; then
	notok "no incompatible ABI change in the postgres binary" "$(cat "$out")"
elif [ $((rc & 1)) -ne 0 ]; then
	notok "abidiff ran" "$(cat "$out")"
elif [ "$rc" -eq 0 ]; then
	ok "no incompatible ABI change, and nothing but additions"
else
	# --no-added-syms was passed, so anything left is a change to something
	# that already existed.
	notok "the series changes symbols that already existed" "$(cat "$out")"
fi

# The added symbols are the point of the series, so show them.
added="$WORKDIR/abidiff-added.txt"
abidiff "$PG_VANILLA/bin/postgres" "$PG_PATCHED/bin/postgres" > "$added" 2>&1
n=$(grep -cE '^ +\[A\]' "$added" 2>/dev/null || echo 0)
if [ "$n" -gt 0 ]; then
	ok "the series adds $n symbol(s), and changes none"
	grep -E '^ +\[A\]' "$added" | sed 's/^/           /'
fi
