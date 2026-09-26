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
# gpMgmt's tools (pg19/gpMgmt) for a suite that runs them -- this one, and
# the suites whose tests run Cloudberry's gpconfig, gpstop, gprecoverseg or
# gpinitstandby on the suite's own cluster, as they run them on Cloudberry's
# demo cluster.  Sourced by a suite's run.sh, with BINDIR the server's bin
# directory, and given the suite's directory of what it executes, whose bin/
# goes first on PATH:
#
#     . "$HERE/../gpmgmt/tools.sh" "$EXEC" || { echo "...; skipping"; exit 77; }
#
# It fails where gpMgmt, or the Python it needs, is not installed.  The tools
# are in the server's prefix, GPHOME, with the environment they are run in,
# cloudberry-env.sh; they name the user they run as (USER); and they reach a
# node's host with ssh even where it is this one -- ssh in the bin/ given is
# this directory's, which runs the command here, as every node of a suite's
# cluster is on this host.  They connect to database template1, where gp_core
# must be, and find a node by the host gp_segment_configuration gives it.

GPHOME="$(dirname "$BINDIR")"
if [ ! -x "$GPHOME/bin/gpstop" ] || [ ! -f "$GPHOME/cloudberry-env.sh" ] ||
   ! python3 -c 'import pgdb, psutil' 2> /dev/null; then
	return 1
fi
# shellcheck source=/dev/null
. "$GPHOME/cloudberry-env.sh"
mkdir -p "$1/bin"
cp "$(dirname "${BASH_SOURCE[0]}")/ssh" "$1/bin/ssh"
chmod +x "$1/bin/ssh"
export PATH="$1/bin:$PATH" USER="${USER:-$(id -un)}"
