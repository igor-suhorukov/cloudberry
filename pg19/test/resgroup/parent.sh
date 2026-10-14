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
# The cgroup parent of a resource group test's cluster, in the tests and in
# their expected output: the rules of sed that put it where Cloudberry's suite
# names its own, gpdb.  Sourced by this suite's run.sh, a cluster's, and by
# ../singlenode_resgroup's, one node's.
#
#     resgroup_parent_sed <parent> [<cores>]
#
# Cloudberry's gpdb, as the parent given: in a path, in a helper's
# get_cgroup_prop(), in the setting gpconfig is given, and in the row SHOW
# prints it in, whose width stays the column's; and every core, as a shell
# command of a test counts them, as the cores given, where the job has cores
# of its own.  And the helper that finds on which cores a group's processes
# run reads the parent's processes (cb_ps.py), where Cloudberry's runs ps,
# which reads every process of the machine, once every 10 ms -- here the
# processes of every job of a full run, which took minutes.
resgroup_parent_sed() {
	local parent="$1" cpus="${2:-}" dir pad
	dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	pad=$(printf '%*s' $(( ${#parent} - 4 )) '')
	echo "s#/sys/fs/cgroup/gpdb\\b#/sys/fs/cgroup/$parent#g"
	echo "s#get_cgroup_prop\\('gpdb/#get_cgroup_prop('$parent/#g"
	echo "s#(cgroup_parent -v )\"gpdb\"#\\1\"$parent\"#g"
	echo "s#^ gpdb$pad( *)\$# $parent\\1#"
	[ -n "$cpus" ] && echo "s#0-\\$\\(\\(\\$\\(nproc\\)-1\\)\\)#$cpus#g"
	echo "s#subprocess\\.check_output\\(\\['ps', '-eF'\\]\\)#(__import__('sys').path.__contains__('$dir') or __import__('sys').path.insert(0, '$dir'), __import__('cb_ps'))[1].ps('$parent')#g"
}
