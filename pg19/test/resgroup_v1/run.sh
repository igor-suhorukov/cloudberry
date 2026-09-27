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
# Cloudberry's resource group tests on cgroup v1.
#
# src/test/isolation2's isolation2_resgroup_v1_schedule, through the
# isolation2 suite's harness (../isolation2/run.sh), as ../resgroup runs the
# v2 schedule.  Its first test, resgroup_auxiliary_tools_v1, turns resource
# groups on as Cloudberry's own suite does: gpconfig sets gp_resource_manager
# to group -- cgroup v1 -- and gpstop restarts the cluster.  Its tests read
# and write the parent gpdb in each of cgroup v1's hierarchies, cpu,
# cpuacct, cpuset and memory, as Cloudberry's hosts had them: which the
# kernels of these hosts cannot mount, in a container or out of one
# (CONFIG_CPUSETS_V1 and CONFIG_MEMCG_V1 off, 6.12's defaults), so the suite
# runs in a KVM guest booted with a kernel that can, whose init mounts them
# and gives the parents to postgres (../../docker/cgroup-v1-vm.sh).
# Elsewhere it is skipped.  One cluster at a time, the groups of the
# manifest one after the other, the tests of a group in the schedule's
# order; a difference kept is here or, for a test of the v2 schedule's too
# that answers alike, in ../resgroup/cloudberry.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for c in cpu cpuacct cpuset memory; do
	if [ ! -w "/sys/fs/cgroup/$c/gpdb/cgroup.procs" ]; then
		echo "resgroup_v1: cgroup v1's /sys/fs/cgroup/$c/gpdb is not there for this user to write; skipping (../../docker/cgroup-v1-vm.sh runs it)"
		exit 77
	fi
done

all_groups=$(awk '$1 == "run" && $3 != "*" && !seen[$3]++ { print $3 }' "$HERE/manifest")

rc=0
for pass in ${PASSES:-planner orca}; do
for group in ${RG_GROUPS:-$all_groups}; do
	manifest="$(mktemp "${TMPDIR:-/tmp}/cb-resgroup-v1-XXXXXX.manifest")"
	awk -v g="$group" '$1 != "run" || $3 == g || $3 == "*"' "$HERE/manifest" > "$manifest"

	# As Cloudberry's installcheck-resgroup-v1 runs it: in its own database,
	# and compared under init_file_resgroup, beside the port's init_file of
	# the v2 schedule's
	PASSES="$pass" ISOLATION2_SUITE=resgroup_v1 ISOLATION2_MANIFEST="$manifest" \
	ISOLATION2_KEPT="$HERE/cloudberry $HERE/../resgroup/cloudberry" \
	ISOLATION2_SCHEDULE_NAME=isolation2_resgroup_v1_schedule \
	ISOLATION2_DBNAME=isolation2resgrouptest \
	ISOLATION2_CB_INIT="${CB_ISOLATION2_DIR:-/cb/src/test/isolation2}/init_file_resgroup" \
	ISOLATION2_EXTRA_INIT="$HERE/../resgroup/init_file" \
		bash "$HERE/../isolation2/run.sh"
	st=$?
	rm -f "$manifest"
	if [ "$st" -ne 0 ] && { [ "$rc" -eq 0 ] || [ "$rc" -eq 77 ]; }; then
		rc=$st
	fi
done
done
exit "$rc"
