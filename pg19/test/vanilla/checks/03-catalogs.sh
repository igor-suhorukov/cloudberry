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
#
# Check 3: catalogs, settings and everything else a fresh cluster describes
# about itself.
#
# The rule says the patch series adds no catalog row, no setting, no wait
# event, no keyword, and changes no stored parse tree.  After initdb the two
# builds therefore have to describe themselves identically, which is what this
# compares -- output by output, so that a difference names itself.
#
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

vport=$(port_for catalogs 0)
pport=$(port_for catalogs 1)
vdata="$WORKDIR/cat-vanilla"
pdata="$WORKDIR/cat-patched"

for pair in "vanilla:$PG_VANILLA:$vdata:$vport" "patched:$PG_PATCHED:$pdata:$pport"; do
	IFS=: read -r tag prefix data port <<< "$pair"
	if ! pg_init "$prefix" "$data"; then
		notok "initdb ($tag)" "$(tail -10 "$data.initdb.log")"
		exit 1
	fi
	if ! pg_start "$prefix" "$data" "$port"; then
		notok "server starts ($tag)" "$(tail -10 "$data.log")"
		exit 1
	fi
done
trap 'pg_stop "$PG_VANILLA" "$vdata"; pg_stop "$PG_PATCHED" "$pdata"' EXIT

ok "both builds ran initdb and started"

###############################################################################
# Things the server reports over SQL
###############################################################################
probe() {								# probe <label> <sql>
	psql_at "$PG_VANILLA" "$vport" "$2" > "$WORKDIR/v.out"
	psql_at "$PG_PATCHED" "$pport" "$2" > "$WORKDIR/p.out"
	compare "$1" "$WORKDIR/v.out" "$WORKDIR/p.out"
}

for cat in pg_proc pg_type pg_class pg_attribute; do
	probe "$cat is identical" "SELECT * FROM $cat ORDER BY oid::text, ctid::text;"
done

# The definition of every setting, not its current value: the values differ by
# port and directory because this script set them.
probe "pg_settings: names, contexts, types, defaults and limits" \
	"SELECT name, context, vartype, unit, boot_val, min_val, max_val, enumvals
	   FROM pg_settings ORDER BY name;"

probe "pg_wait_events is identical" \
	"SELECT type, name, description FROM pg_wait_events ORDER BY type, name;"

probe "pg_get_keywords() is identical" \
	"SELECT * FROM pg_get_keywords() ORDER BY word;"

probe "version() is identical" "SELECT version();"

# Stored parse trees.  Query IDs and pg_node_tree values are generated from the
# node definitions, so a new node field would show up here.
probe "pg_rewrite.ev_action is identical" \
	"SELECT ev_class::regclass::text, rulename, ev_action
	   FROM pg_rewrite ORDER BY 1, 2;"
probe "pg_proc.prosqlbody is identical" \
	"SELECT oid, prosqlbody FROM pg_proc
	  WHERE prosqlbody IS NOT NULL ORDER BY oid;"
probe "pg_attrdef.adbin is identical" \
	"SELECT adrelid::regclass::text, adnum, adbin FROM pg_attrdef ORDER BY 1, 2;"

###############################################################################
# Things the binaries report
###############################################################################
pg_run "$PG_VANILLA" postgres --describe-config > "$WORKDIR/v.out" 2>&1
pg_run "$PG_PATCHED" postgres --describe-config > "$WORKDIR/p.out" 2>&1
compare "postgres --describe-config is identical" "$WORKDIR/v.out" "$WORKDIR/p.out"

# pg_controldata, restricted to the fields that describe the format.  The rest
# -- identifiers, times, WAL positions -- differ between any two clusters.
control_format() {						# control_format <prefix> <datadir>
	pg_run "$1" pg_controldata -D "$2" | grep -E \
		'^(pg_control version|Catalog version|Maximum data alignment|Database block size|Blocks per segment|WAL block size|Bytes per WAL segment|Maximum length of identifiers|Maximum columns in an index|Maximum size of a TOAST chunk|Size of a large-object chunk|Date/time type storage|Float8 argument passing|Data page checksum version|Default char data signedness)'
}
control_format "$PG_VANILLA" "$vdata" > "$WORKDIR/v.out"
control_format "$PG_PATCHED" "$pdata" > "$WORKDIR/p.out"
compare "pg_controldata reports the same format" "$WORKDIR/v.out" "$WORKDIR/p.out"
