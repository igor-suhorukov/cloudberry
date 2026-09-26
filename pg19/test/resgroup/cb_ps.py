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
# What `ps -eF` says of the processes of a cluster's resource groups, for a
# resource group test's helper that looks on which cores a group's processes
# run: its PSR and CMD columns, of the processes in the cgroups under the
# cluster's parent alone (run.sh).
#
# The helper, check_cpuset() of resgroup_auxiliary_tools_v2, runs ps -eF once
# every 10 ms for ten seconds; ps reads every process of the machine, even
# when it is given their pids, which in a full run of the port's suites are
# thousands -- 50 ms a call, and minutes a test.  This reads the few in the
# parent's cgroups, and passes over one that ends meanwhile.

import glob


def ps(parent):
    lines = [b'PSR CMD']
    root = '/sys/fs/cgroup/%s' % parent
    for procs in glob.glob(root + '/*/cgroup.procs') + glob.glob(root + '/*/*/cgroup.procs'):
        try:
            with open(procs) as f:
                pids = f.read().split()
        except OSError:
            continue
        for pid in pids:
            try:
                with open('/proc/%s/stat' % pid, 'rb') as f:
                    stat = f.read()
                with open('/proc/%s/cmdline' % pid, 'rb') as f:
                    cmdline = f.read()
            except OSError:
                continue
            # the fields after "pid (comm)" begin with the third, state; the
            # 39th is the core the process last ran on
            psr = stat.rsplit(b')', 1)[1].split()[36]
            lines.append(psr + b' ' + cmdline.replace(b'\0', b' ').strip())
    return b'\n'.join(lines)
