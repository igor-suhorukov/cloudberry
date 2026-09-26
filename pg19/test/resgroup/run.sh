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
# gp_resource_group_cgroup_parent to gpdb and gp_resource_manager to
# group-v2, and gpstop restarts the cluster.  So the cgroup file system must
# have /sys/fs/cgroup/gpdb, with the controllers on, for this user to write,
# and the root's cgroup.procs, to move a postmaster in -- the tests service
# sets that up as root, in a privileged container (../cgroup.sh); without
# it the suite has nothing to run against.  The tests read the cgroups under
# gpdb by that name, so one cluster at a time runs them: this suite is one
# job, its passes one after the other.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CG=/sys/fs/cgroup/gpdb

if [ ! -w "$CG/cgroup.procs" ] || [ ! -w /sys/fs/cgroup/cgroup.procs ]; then
	echo "resgroup: $CG, or the root cgroup's cgroup.procs, is not there for this user to write; skipping"
	exit 77
fi

# resgroup_io_limit limits a group's I/O on the disk its tablespace is on,
# which it finds from the mount the directory is on: not the container's
# overlay, nor /tmp, a tmpfs here.  The tests service mounts a volume for it
# (../docker/compose.yml); where it is, the clusters are made on it.
DISK="${CB_TEST_DISK:-/var/lib/cbdisk}"
if [ -d "$DISK" ] && [ -w "$DISK" ]; then
	export TMPDIR="$DISK"
fi

# As Cloudberry's installcheck-resgroup-v2 runs it: in its own database, and
# compared under init_file_resgroup, beside the port's init_file here.
ISOLATION2_SUITE=resgroup ISOLATION2_MANIFEST="$HERE/manifest" \
ISOLATION2_KEPT="$HERE/cloudberry" \
ISOLATION2_SCHEDULE_NAME=isolation2_resgroup_v2_schedule \
ISOLATION2_DBNAME=isolation2resgrouptest \
ISOLATION2_CB_INIT="${CB_ISOLATION2_DIR:-/cb/src/test/isolation2}/init_file_resgroup" \
ISOLATION2_EXTRA_INIT="$HERE/init_file" \
	exec bash "$HERE/../isolation2/run.sh"
