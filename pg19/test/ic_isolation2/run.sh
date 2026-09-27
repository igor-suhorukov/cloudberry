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
# ic_isolation2: isolation2_schedule's ORCA pass, the isolation2 suite's
# (../isolation2), with udp2 or the proxy as every node's interconnect, as it
# runs with tcp in every full run.  The ORCA pass alone: only ORCA's plans
# stream, and the planner's pass never meets a transport (gp_motion.c).
# The harness has each node load the transport's module, sets it as every
# node's gp.interconnect_type -- and the proxies' addresses -- and checks
# that each node has it once the clusters are up.
#
# Run on request only: it takes what the isolation2 suite's ORCA pass takes,
# again for each transport, and the ic suite runs the transports' checks in
# every run.
#
#   CB_IC=udp2|proxy|all   the transports to run it over
#   IC                     one of them, as each job of the suite's is given
#                          it (../jobs)
#
#   docker compose -f pg19/docker/compose.yml run --rm -e CB_IC=all tests ic_greenplum ic_isolation2

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"

case "${CB_IC:-}" in
	udp2|proxy) want="$CB_IC" ;;
	all) want="udp2 proxy" ;;
	*)
		echo "  skipped: the isolation2 suite over udp2 and the proxy runs on request, CB_IC=udp2, proxy or all"
		exit 77 ;;
esac
if [ -n "${IC:-}" ]; then
	if [[ " $want " != *" $IC "* ]]; then
		echo "  skipped: CB_IC=$CB_IC does not ask for $IC"
		exit 77
	fi
	want="$IC"
fi

rc=0
for t in $want; do
	case "$t" in
		udp2) module=udp2 ;;
		proxy) module=interconnect ;;
		*) echo "no interconnect $t"; exit 1 ;;
	esac
	if [ ! -f "$("$BINDIR/pg_config" --pkglibdir)/$module.so" ]; then
		echo "$t is asked for, and its module, $module, is not built"
		rc=1
		continue
	fi
	echo "isolation2's ORCA pass over $t"
	ISOLATION2_PRELOAD_MORE="$module" ISOLATION2_INTERCONNECT="$t" PASSES=orca \
		"$here/../isolation2/run.sh" || rc=1
done
exit $rc
