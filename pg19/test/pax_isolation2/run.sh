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
# PAX's isolation2 schedule, on a cluster.
#
# contrib/pax_storage/src/test/isolation2 is Cloudberry's isolation2 suite as
# PAX keeps it: a schedule of its own, isolation2_schedule, of Cloudberry's
# tests with expected output where PAX answers otherwise, and tests of PAX's
# own (pax/, reindex/ ..._pax_...).  Cloudberry's CI runs it on its demo
# cluster, with PAX the default table access method of every node, under the
# planner and under ORCA (pax-ic-isolation2-opt-off and -opt-on,
# .github/workflows/build-cloudberry.yml; PAX's Makefile, isolation2_test).
#
# The port runs it as the isolation2 suite runs Cloudberry's schedule,
# through its harness (../isolation2/run.sh), in the groups manifest puts the
# tests in: each node loads gp_ao and pax too and has
# default_table_access_method = pax, and the tests' database has both
# extensions and the helpers PAX's setup adds (setup.sql, after the
# isolation2 suite's).  A test the isolation2 suite runs is in the group it
# has there; each test is compared under Cloudberry's init files, PAX's
# copies of them and the port's, or exactly a difference in cloudberry/,
# reviewed and kept -- or in the isolation2 suite's, for a test whose copy
# here answers as Cloudberry's.  PAX's catalogs, which Cloudberry's initdb
# made in pg_ext_aux, are in the schema pax, as the tests are read here, and
# a test's regress.so the isolation2 suite's own path (sed).

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PAX_ISOLATION2="${CB_PAX_ISOLATION2_DIR:-/cb/contrib/pax_storage/src/test/isolation2}"

if [ ! -f "$PAX_ISOLATION2/isolation2_schedule" ] ||
   [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/pax.control" ]; then
	echo "PAX's isolation2 suite or pax is not installed; skipping"
	exit 77
fi

inits="$PAX_ISOLATION2/init_file"
[ -f "$HERE/init_file" ] && inits="$inits $HERE/init_file"
export ISOLATION2_SUITE=pax_isolation2
export ISOLATION2_MANIFEST="$HERE/manifest"
# a difference kept here, or the isolation2 suite's for the same test, where
# PAX's copy of it answers alike
export ISOLATION2_KEPT="$HERE/cloudberry $HERE/../isolation2/cloudberry"
export ISOLATION2_SCHEDULE_NAME="contrib/pax_storage isolation2_schedule"
export ISOLATION2_TESTS_DIR="$PAX_ISOLATION2"
export ISOLATION2_CB_INIT="$PAX_ISOLATION2/init_file_isolation2"
export ISOLATION2_EXTRA_INIT="$inits"
export ISOLATION2_EXTRA_SED="$HERE/sed"
export ISOLATION2_PRELOAD_MORE="gp_ao,pax"
export ISOLATION2_SETTINGS="default_table_access_method = pax"
export ISOLATION2_SETUP_MORE="$HERE/setup.sql"
exec "$HERE/../isolation2/run.sh"
