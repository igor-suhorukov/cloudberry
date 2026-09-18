#-------------------------------------------------------------------------
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
#-------------------------------------------------------------------------

# The port's tests.
#
# The load tests need the modules installed, because they start a real server
# that preloads them.  So they are not wired into "ninja test", which runs
# against the build directory; run them after "ninja install", or through the
# Compose project, which does both:
#
#     docker compose -f pg19/docker/compose.yml run --rm tests load

load_test = files('load/run.sh')

run_target('test-load',
  command: [load_test],
  env: {
    'PG_BINDIR': pg['bindir'],
    'PG_PKGLIBDIR': pg_pkglibdir,
  },
)
