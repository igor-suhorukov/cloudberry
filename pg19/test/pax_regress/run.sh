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
# PAX's copy of Cloudberry's regression suite, on a cluster.
#
# contrib/pax_storage/src/test/regress is Cloudberry's src/test/regress as
# PAX keeps it: PostgreSQL's parallel_schedule and Cloudberry's
# greenplum_schedule, their tests and expected output changed where PAX
# answers otherwise.  Cloudberry's CI runs the two schedules one after the
# other, in one database of its demo cluster, with PAX the default table
# access method of every node, under the planner and under ORCA
# (pax-ic-good-opt-off and -opt-on, .github/workflows/build-cloudberry.yml;
# PAX's Makefile, regress_test).
#
# The port runs them as the greenplum suite runs Cloudberry's own
# greenplum_schedule, through its harness (../greenplum/run.sh): in the
# groups manifest puts them in, each on a coordinator and three segments of
# its own, in two passes.  Each node loads pax too and has
# default_table_access_method = pax.  A group's pass begins with the port's
# setup (gp_setup), PAX in the database and in template1 (sql/pax_setup.sql,
# as Cloudberry's initdb makes it in every database), and then PostgreSQL
# 19's test_setup, whose tables -- onek, tenk1 and the rest the tests read
# -- are PAX's, as Cloudberry's CI has them.  The extensions' own tables are
# heaps whatever the default says.  PostgreSQL's parallel_schedule is
# PostgreSQL 14's tests as Cloudberry keeps them, which no suite of the
# port's has run on a cluster before: its tests run in a group of their
# own, pg, in the schedule's order.  The greenplum_schedule tests are in
# the groups, and at the places among the port's tests, that the greenplum
# suite's manifest gives them; a test that suite skips is skipped here for
# the same reason, where PAX's copy of it reaches the same statement.
#
# Each test is compared as the greenplum suite compares it, under
# Cloudberry's init_file, PAX's copy of it and the port's; a difference
# reviewed and kept is in cloudberry/.  PAX's catalogs, which Cloudberry's
# initdb made in pg_ext_aux, are in the schema pax, as the tests are read
# here (respell).

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PAX_REGRESS="${CB_PAX_REGRESS_DIR:-/cb/contrib/pax_storage/src/test/regress}"

if [ ! -f "$PAX_REGRESS/greenplum_schedule" ] ||
   [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/pax.control" ]; then
	echo "PAX's regression suite or pax is not installed; skipping"
	exit 77
fi

export CB_REGRESS_DIR="$PAX_REGRESS"
export CB_GPDIFF_DIR="${CB_GPDIFF_DIR:-/cb/src/test/regress}"
export GP_SUITE_DIR="$HERE"
export GP_TITLE="pax_regress: PAX's copy of Cloudberry's regression suite"
export GP_PRELOAD_MORE=pax
export GP_SETTINGS="default_table_access_method = pax"
export GP_SETUP="gp_setup pax_setup test_setup"
exec "$HERE/../greenplum/run.sh"
