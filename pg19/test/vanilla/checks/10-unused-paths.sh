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
# Check 10: every added statement inside an existing function is guarded.
#
# The rule behind the whole series is that an unused hook leaves the code path
# exactly as it was.  This reads the series itself and asks, of every line it
# adds to a function that already existed, whether reaching that line depends
# on something the series introduced -- a hook pointer, a flag or a registry.
#
# It is a reading aid, not a proof: it classifies each added line and prints
# the ones it cannot account for, which are the ones worth reading by hand.
#
# It needs the PostgreSQL repository, not an installation:
#
#   PG_SRC_REPO=/path/to/postgres  [PG_BASE_REF=...] [PG_SERIES_REF=...]
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

repo="${PG_SRC_REPO:-}"
base="${PG_BASE_REF:-origin/REL_19_STABLE}"
head="${PG_SERIES_REF:-REL_19_STABLE_CLOUDBERRY}"

if [ -z "$repo" ] || [ ! -d "$repo/.git" ]; then
	skip "unused-path review" "PG_SRC_REPO is not a git repository"
	exit 0
fi
if ! git -C "$repo" rev-parse --verify -q "$base" > /dev/null \
   || ! git -C "$repo" rev-parse --verify -q "$head" > /dev/null; then
	skip "unused-path review" "cannot resolve $base..$head in $repo"
	exit 0
fi

out="$WORKDIR/unused-paths.txt"
PG_REPO="$repo" PG_BASE="$base" PG_HEAD="$head" \
	python3 "$(dirname "${BASH_SOURCE[0]}")/unused_paths.py" > "$out" 2>&1
rc=$?

head -12 "$out" | sed 's/^/         /'

if [ $rc -eq 0 ]; then
	ok "every added statement in an existing function is gated by the series"
else
	notok "added statements that need reading by hand" \
		"$(sed -n '/^added statements/,$p' "$out" | head -24)"
fi
