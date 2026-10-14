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
# Cloudberry's resource group tests, on one node.
#
# src/test/singlenode_isolation2 has a schedule of them,
# isolation2_resgroup_schedule, thirty tests, which nothing of Cloudberry's
# runs and its server would not take: the cluster's schedule and files as
# they were when the single-node suite was made, of options its resource
# groups have not had since (manifest says which, and what became of each
# test).  What that schedule is on a cluster today is
# isolation2_resgroup_v2_schedule, which ../resgroup runs on a coordinator
# and three segments; this suite runs it on one node, through the
# single-node isolation2 suite's harness (../singlenode_isolation2/run.sh):
# each test from src/test/isolation2, compared with its expected output
# there -- the cluster's, which one node answers alike, a transaction of a
# role running in the role's group on one node as on a cluster's coordinator
# (gp_resource's resgroup.c) -- with the manifest here.  Its first test,
# resgroup_auxiliary_tools_v2, turns resource groups on as it does on a
# cluster: gpconfig sets gp_resource_group_cgroup_parent and
# gp_resource_manager to group-v2, and gpstop restarts the node.
#
# As in ../resgroup, each group of the manifest in each pass has a node and
# a cgroup parent of its own, gpdb_<pass>_single_<group>, which the tests
# and their expected output name where Cloudberry's name gpdb, so that they
# run side by side, as jobs of a full run (../jobs); RG_GROUPS says which
# groups run, all of them, one after the other, when it is not set.  The
# cgroup file system must have the parent, with the controllers on, for
# this user to write, and the root's cgroup.procs -- the tests service sets
# that up (../cgroup.sh); without it the group has nothing to run against.
# CPUS gives a group's job cores of its own, as there: the group cpu runs in
# each pass on half the cores, both passes by themselves after the other
# jobs, and after the cluster's group cpu, which has the same cores
# (../jobs, ALONE=2).
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../resgroup/parent.sh"

# The node on a disk, as ../resgroup's clusters are, for resgroup_io_limit.
DISK="${CB_TEST_DISK:-/var/lib/cbdisk}"
if [ -d "$DISK" ] && [ -w "$DISK" ]; then
	export TMPDIR="$DISK"
fi

CBI2="${CB_CLUSTER_ISOLATION2_DIR:-/cb/src/test/isolation2}"
MANIFEST="${SN_RESGROUP_MANIFEST:-$HERE/manifest}"

# The manifest's groups, in the order they first appear; "*" is every group.
all_groups=$(awk '$1 == "run" && $3 != "*" && !seen[$3]++ { print $3 }' "$MANIFEST")

rc=0
for pass in ${PASSES:-planner orca}; do
for group in ${RG_GROUPS:-$all_groups}; do
	parent="gpdb_${pass}_single_$group"
	CG="/sys/fs/cgroup/$parent"
	if [ ! -w "$CG/cgroup.procs" ] || [ ! -w /sys/fs/cgroup/cgroup.procs ]; then
		echo "singlenode_resgroup: $CG, or the root cgroup's cgroup.procs, is not there for this user to write; skipping group $group of the $pass pass"
		[ "$rc" -eq 0 ] && rc=77
		continue
	fi
	# the group's tests; and what is skipped, said with the first group's
	manifest="$(mktemp "${TMPDIR:-/tmp}/cb-snresgroup-XXXXXX.manifest")"
	awk -v g="$group" -v first="${all_groups%%[[:space:]]*}" \
		'$1 == "skip" ? g == first : $1 != "run" || $3 == g || $3 == "*"' "$MANIFEST" > "$manifest"

	# Cloudberry's gpdb, as the node's parent, and every core as the job's
	# own (../resgroup/parent.sh); and what the isolation2 suite's harness
	# respells for these tests on a cluster (see there): no utility mode but
	# a node's own connection, the wait of a group's waiter by its name,
	# get_tablespace_path()'s result as a pointer, and the superuser in the
	# environment a helper makes for psql.  And the directory a test makes
	# a tablespace in, /tmp/testtablespace, as one of the group's own where
	# the node is: resgroup_io_limit's, on the disk -- the cluster's harness
	# gives a group every /tmp/<name> of a test so.
	sed="$(mktemp "${TMPDIR:-/tmp}/cb-snresgroup-XXXXXX.sed")"
	spc="$(mktemp -d "${TMPDIR:-/tmp}/cb-snresgroup-XXXXXX.spc")"
	{
		resgroup_parent_sed "$parent" "${CPUS:-}"
		echo "s#/tmp/testtablespace\\b#$spc#g"
		echo "s/-c gp_role=utility//g"
		echo "s/wait_event_type\\s*=\\s*'ResourceGroup'/wait_event='ResourceGroup'/g"
		echo "s/(get_tablespace_path = postgres\\['get_tablespace_path'\\])/\\1; get_tablespace_path.restype = ctypes.c_void_p/"
		echo 's/"PATH": os\.getenv\("PATH"\)/"PATH": os.getenv("PATH"), "PGUSER": os.getenv("PGUSER", "gpadmin")/'
	} > "$sed"

	# As Cloudberry's installcheck-resgroup-v2 runs it: in its own database,
	# as gpadmin, compared under init_file_resgroup and the port's init file
	# of the cluster's suite; a difference kept is the cluster's, the node
	# answering as it does -- ../resgroup/cloudberry's, and for the v1
	# schedule's test ../resgroup_v1/cloudberry's.  The database postgres
	# has gp_resource too, as the cluster's has: a helper makes a group
	# there.
	PASSES="$pass" SN_ISOLATION2_SUITE=singlenode_resgroup \
	SN_ISOLATION2_WHAT="Cloudberry's resource group tests, on one node" \
	SN_ISOLATION2_MANIFEST="$manifest" \
	SN_ISOLATION2_KEPT="$HERE/../resgroup/cloudberry $HERE/../resgroup_v1/cloudberry" SN_ISOLATION2_OWN="$HERE" \
	SN_ISOLATION2_TESTS_DIR="$CBI2" \
	SN_ISOLATION2_DBNAME=isolation2resgrouptest SN_ISOLATION2_SUPERUSER=gpadmin \
	SN_ISOLATION2_POSTGRES_MORE=gp_resource \
	SN_ISOLATION2_CB_INIT="$CBI2/init_file_resgroup" \
	SN_ISOLATION2_EXTRA_INIT="$HERE/../resgroup/init_file" \
	SN_ISOLATION2_KINDS="setsys altersys func nameeq showguc like header0" \
	SN_ISOLATION2_EXTRA_SED="$sed" \
		${CPUS:+taskset -c "$CPUS"} bash "$HERE/../singlenode_isolation2/run.sh"
	st=$?
	rm -rf "$sed" "$manifest" "$spc"
	if [ "$st" -ne 0 ] && { [ "$rc" -eq 0 ] || [ "$rc" -eq 77 ]; }; then
		rc=$st
	fi
done
done
exit "$rc"
