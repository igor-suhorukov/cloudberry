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
# The tests service's entrypoint: as root, a cgroup v2 subtree for the
# resource group suite -- /sys/fs/cgroup/gpdb, with the controllers on, the
# test user's to write -- and then the suites, as that user (run.sh).
#
# Cloudberry's resource group tests run a cluster whose
# gp_resource_group_cgroup_parent is gpdb, a cgroup it writes its groups
# into.  On a host that is systemd's to delegate; in a container it is the
# container's own cgroup namespace, which it may write when it is privileged
# (../docker/compose.yml).  A cgroup that holds processes cannot hand its
# controllers down, so every process of the container's root moves into a
# leaf of its own, init, first; and a postmaster moves itself from there into
# gpdb's system group, which asks write access to the common ancestor's
# cgroup.procs, the root's.  Where the cgroup file system is not writable
# nothing is set up, and the resgroup suite says it has nothing to run
# against.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
user="${CB_TEST_USER:-postgres}"
cg=/sys/fs/cgroup

if [ "$(id -u)" -ne 0 ]; then
	exec "$here/run.sh" "$@"
fi

if [ -f "$cg/cgroup.controllers" ] && [ -w "$cg/cgroup.procs" ] &&
   mkdir -p "$cg/init" 2> /dev/null; then
	while read -r pid; do
		echo "$pid" > "$cg/init/cgroup.procs" 2> /dev/null || true
	done < "$cg/cgroup.procs"
	echo "+cpuset +cpu +io +memory +pids" > "$cg/cgroup.subtree_control" 2> /dev/null &&
	mkdir -p "$cg/gpdb" &&
	echo "+cpuset +cpu +io +memory +pids" > "$cg/gpdb/cgroup.subtree_control" 2> /dev/null &&
	chown -R "$user" "$cg/gpdb" &&
	chmod a+w "$cg/cgroup.procs" ||
		echo "cgroup.sh: could not set up $cg/gpdb; the resgroup suite will skip" >&2
fi

# the disk the resource group suite makes its clusters on
# (resgroup/run.sh), where the tests service mounts one
[ -d /var/lib/cbdisk ] && chown "$user" /var/lib/cbdisk

HOME="$(getent passwd "$user" | cut -d: -f6)"
export HOME
exec setpriv --reuid="$user" --regid="$user" --init-groups "$here/run.sh" "$@"
