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
# Cloudberry's gpexpand and gpshrink, on a cluster: M8's.
#
# src/test/isolation2's isolation2_expandshrink_schedule, whose one test,
# gpexpand_gpshrink, adds a segment with its mirror to a running cluster
# with gpexpand and takes it away with gpshrink, three times: its tables'
# rows spread over the new segment, and back.  As Cloudberry's CI runs it
# (ic-expandshrink): a job of its own, on a cluster of its own -- every table
# of every database is moved -- through the isolation2 suite's harness
# (../isolation2/run.sh), in its own database, with the manifest here.  The
# cluster is the harness's with mirrors and a standby, Cloudberry's demo
# cluster, and the segments the test adds are its own: localhost at the demo
# cluster's ports 7008 and after, in the test's input files, are the group's
# socket directory at ports of its own, in the test and its expected output
# alike, as the harness respells them.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ISOLATION2_SUITE=expandshrink ISOLATION2_MANIFEST="$HERE/manifest" \
ISOLATION2_KEPT="$HERE/cloudberry" \
ISOLATION2_SCHEDULE_NAME=isolation2_expandshrink_schedule \
ISOLATION2_DBNAME=isolation2expandshrinktest \
	exec bash "$HERE/../isolation2/run.sh"
