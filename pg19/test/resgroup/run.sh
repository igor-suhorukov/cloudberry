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
# Cloudberry's resource group tests, on a cluster: M6's.
#
# src/test/isolation2's isolation2_resgroup_v2_schedule -- the tests of
# resource groups on cgroup v2, which Cloudberry's hosts run -- through the
# isolation2 suite's harness (../isolation2/run.sh), with the manifest and
# the reviewed differences here.  Its first test, resgroup_auxiliary_tools_v2,
# turns resource groups on as Cloudberry's own suite does: gpconfig sets
# gp_resource_group_cgroup_parent and gp_resource_manager to group-v2, and
# gpstop restarts the cluster.
#
# The tests write the cgroups under their cluster's parent, and read them by
# its name, which is gpdb in Cloudberry's suite.  Here each group of the
# manifest in each pass has a cluster and a parent of its own,
# gpdb_<pass>_<group>, so that they run side by side, as jobs of a full run
# (../jobs): the tests and their expected output name the parent where
# Cloudberry's name gpdb.  RG_GROUPS says which groups run, all of them, one
# after the other, when it is not set.  The cgroup file system must have the
# parent, with the controllers on, for this user to write, and the root's
# cgroup.procs, to move a postmaster in -- the tests service sets that up as
# root, in a privileged container (../cgroup.sh); without it the group has
# nothing to run against.
#
# CPUS gives a group's job cores of its own: its servers and the tests' own
# processes run on them alone (taskset), since Cloudberry reckons a group's
# share of the CPU against the cores its postmaster may run on.  The group
# cpu, whose test keeps its cores busy and measures what a group gets of
# them, runs in each pass on half the cores, both passes by themselves after
# the other jobs (../jobs, ALONE): on fewer cores, or beside other jobs, a
# group's share missed the test's ten points, and the test's own statements,
# a superuser's in admin_group at a hundredth of the cores, came too late to
# cancel what they started.  And the helper that finds on which cores a
# group's processes run lists the parent's processes, where Cloudberry's
# lists every process of the machine, once every 10 ms -- here the processes
# of every job of a full run, which took minutes.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# resgroup_io_limit limits a group's I/O on the disk its tablespace is on,
# which it finds from the mount the directory is on: not the container's
# overlay, nor /tmp, a tmpfs here.  The tests service mounts a volume for it
# (../docker/compose.yml); where it is, the clusters are made on it.
DISK="${CB_TEST_DISK:-/var/lib/cbdisk}"
if [ -d "$DISK" ] && [ -w "$DISK" ]; then
	export TMPDIR="$DISK"
fi

# The manifest's groups, in the order they first appear; "*" is every group.
all_groups=$(awk '$1 == "run" && $3 != "*" && !seen[$3]++ { print $3 }' "$HERE/manifest")

rc=0
for pass in ${PASSES:-planner orca}; do
for group in ${RG_GROUPS:-$all_groups}; do
	parent="gpdb_${pass}_$group"
	CG="/sys/fs/cgroup/$parent"
	if [ ! -w "$CG/cgroup.procs" ] || [ ! -w /sys/fs/cgroup/cgroup.procs ]; then
		echo "resgroup: $CG, or the root cgroup's cgroup.procs, is not there for this user to write; skipping group $group of the $pass pass"
		[ "$rc" -eq 0 ] && rc=77
		continue
	fi
	manifest="$(mktemp "${TMPDIR:-/tmp}/cb-resgroup-XXXXXX.manifest")"
	awk -v g="$group" '$1 != "run" || $3 == g || $3 == "*"' "$HERE/manifest" > "$manifest"

	# Cloudberry's gpdb, as the pass's parent: in a path, a helper's
	# get_cgroup_prop(), the setting gpconfig is given, and the row SHOW
	# prints it in, whose width stays the column's; and every core, as a
	# shell command of a test counts them, as the parent's.
	cpus="${CPUS:-}"
	procs="cat /sys/fs/cgroup/$parent/*/cgroup.procs /sys/fs/cgroup/$parent/*/*/cgroup.procs 2> /dev/null | paste -sd, -"
	sed="$(mktemp "${TMPDIR:-/tmp}/cb-resgroup-XXXXXX.sed")"
	pad=$(printf '%*s' $(( ${#parent} - 4 )) '')
	{
		echo "s#/sys/fs/cgroup/gpdb\\b#/sys/fs/cgroup/$parent#g"
		echo "s#get_cgroup_prop\\('gpdb/#get_cgroup_prop('$parent/#g"
		echo "s#(cgroup_parent -v )\"gpdb\"#\\1\"$parent\"#g"
		echo "s#^ gpdb$pad( *)\$# $parent\\1#"
		[ -n "$cpus" ] && echo "s#0-\\$\\(\\(\\$\\(nproc\\)-1\\)\\)#$cpus#g"
		echo "s#subprocess\\.check_output\\(\\['ps', '-eF'\\]\\)#subprocess.check_output(['sh', '-c', 'ps -F -p \"\$($procs)\"'])#g"
	} > "$sed"

	# As Cloudberry's installcheck-resgroup-v2 runs it: in its own database,
	# and compared under init_file_resgroup, beside the port's init_file here.
	PASSES="$pass" ISOLATION2_SUITE=resgroup ISOLATION2_MANIFEST="$manifest" \
	ISOLATION2_KEPT="$HERE/cloudberry" \
	ISOLATION2_SCHEDULE_NAME=isolation2_resgroup_v2_schedule \
	ISOLATION2_DBNAME=isolation2resgrouptest \
	ISOLATION2_CB_INIT="${CB_ISOLATION2_DIR:-/cb/src/test/isolation2}/init_file_resgroup" \
	ISOLATION2_EXTRA_INIT="$HERE/init_file" \
	ISOLATION2_EXTRA_SED="$sed" \
		${cpus:+taskset -c "$cpus"} bash "$HERE/../isolation2/run.sh"
	st=$?
	rm -f "$sed" "$manifest"
	if [ "$st" -ne 0 ] && { [ "$rc" -eq 0 ] || [ "$rc" -eq 77 ]; }; then
		rc=$st
	fi
done
done
exit "$rc"
