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
# M0 load tests.
#
# What this checks is the whole of milestone M0: every module builds and loads,
# and the modules that may only be preloaded say so instead of half loading.
# It starts real servers, so it needs the modules installed.
#
#     PG_BINDIR=/path/to/pg19/bin pg19/test/load/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"

# The tools link against this installation's libpq, which is not where the
# loader looks by default when PostgreSQL is installed under its own prefix.
PG_LIBDIR="$("$BINDIR/pg_config" --libdir)"
export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DYLD_LIBRARY_PATH="$PG_LIBDIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
PGDATA_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-m0-XXXXXX")"
PORT="${PGPORT:-$((5600 + RANDOM % 300))}"
export PGPORT="$PORT"
export PGHOST="$PGDATA_ROOT"

PRELOAD_ALL='gp_core,interconnect,gp_orca,gp_ao,pax,gp_matview,gp_task,gp_sql,gp_security'
EXTENSIONS='gp_core gp_orca gp_ao pax gp_exttable gp_resource gp_security gp_task gp_matview gp_sql'

pass=0; fail=0

ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; fail=$((fail + 1)); }

cleanup() {
	"$BINDIR/pg_ctl" -D "$PGDATA_ROOT/data" -m immediate stop >/dev/null 2>&1
	rm -rf "$PGDATA_ROOT"
}
trap cleanup EXIT

start_with() {			# start_with <shared_preload_libraries>; 0 if it came up
	"$BINDIR/pg_ctl" -D "$PGDATA_ROOT/data" -m immediate stop >/dev/null 2>&1
	: > "$PGDATA_ROOT/data/postgresql.auto.conf"
	{
		echo "shared_preload_libraries = '$1'"
		echo "unix_socket_directories = '$PGDATA_ROOT'"
		echo "listen_addresses = ''"
		echo "port = $PORT"
	} >> "$PGDATA_ROOT/data/postgresql.auto.conf"
	"$BINDIR/pg_ctl" -D "$PGDATA_ROOT/data" -l "$PGDATA_ROOT/log" -w -t 30 start >/dev/null 2>&1
}

q() { "$PSQL" -X -q -t -A -d postgres -c "$1" 2>&1; }

echo "M0 load tests"
echo "  bindir   $BINDIR"
echo "  pgdata   $PGDATA_ROOT/data"
echo

"$BINDIR/initdb" -D "$PGDATA_ROOT/data" -N --locale=C --encoding=UTF8 \
	> "$PGDATA_ROOT/initdb.log" 2>&1
if [ $? -ne 0 ]; then
	echo "initdb failed:"; tail -20 "$PGDATA_ROOT/initdb.log"; exit 1
fi

###############################################################################
echo "1. the preload-only modules refuse to load any other way"
###############################################################################
if start_with ''; then
	for m in gp_core interconnect gp_orca gp_ao pax gp_matview gp_task gp_sql gp_security; do
		out=$(q "LOAD '$m';")
		case "$out" in
			*"can only be loaded through \"shared_preload_libraries\""*)
				ok "LOAD '$m' is refused by its own check" ;;
			*"requires \"gp_core\" to be loaded first"*)
				ok "LOAD '$m' is refused: it needs gp_core" ;;
			*"undefined symbol"*)
				# A module that uses gp_core's code is refused earlier still,
				# by the loader, because PostgreSQL resolves those symbols at
				# load time -- so _PG_init never runs to give a nicer message.
				# Still a refusal, and the reason is in the message.
				ok "LOAD '$m' is refused by the loader: gp_core is not there" ;;
			*) notok "LOAD '$m' should have been refused" "$out" ;;
		esac
	done
else
	notok "server starts with no preloaded modules" "$(tail -3 "$PGDATA_ROOT/log")"
fi

###############################################################################
echo "2. a module listed before gp_core says so, rather than failing to link"
###############################################################################
if start_with 'interconnect'; then
	notok "server should not have started with interconnect before gp_core"
else
	if grep -q 'requires "gp_core" to be loaded first' "$PGDATA_ROOT/log"; then
		ok "startup fails and names gp_core"
	else
		notok "startup failed for the wrong reason" "$(tail -3 "$PGDATA_ROOT/log")"
	fi
fi

###############################################################################
echo "3. every module loads when it is preloaded, in the right order"
###############################################################################
if start_with "$PRELOAD_ALL"; then
	ok "server starts with $PRELOAD_ALL"

	loaded=$(q "SELECT string_agg(module_name, ',' ORDER BY module_name) FROM pg_get_loaded_modules();")
	for m in gp_core interconnect gp_orca gp_ao pax gp_matview gp_task gp_sql gp_security; do
		case ",$loaded," in
			*",$m,"*) ok "$m is loaded" ;;
			*)        notok "$m is not in pg_get_loaded_modules()" "$loaded" ;;
		esac
	done

	###########################################################################
	echo "4. gp_core's settings are there, and are named gp.*"
	###########################################################################
	out=$(q "SELECT current_setting('gp.role');")
	[ "$out" = "utility" ] && ok "gp.role defaults to utility" \
		|| notok "gp.role" "$out"

	out=$(q "SELECT count(*) FROM pg_settings WHERE name LIKE 'gp.%';")
	[ "$out" -ge 2 ] 2>/dev/null && ok "gp.* settings are in pg_settings ($out)" \
		|| notok "gp.* settings" "$out"

	###########################################################################
	echo "5. CREATE EXTENSION works for every module that has one"
	###########################################################################
	# Assert on the effect rather than on the command tag, which psql -q
	# does not print.
	for e in $EXTENSIONS; do
		out=$(q "CREATE EXTENSION $e CASCADE;")
		if [ "$(q "SELECT count(*) FROM pg_extension WHERE extname = '$e';")" = "1" ]; then
			ok "CREATE EXTENSION $e"
		else
			notok "CREATE EXTENSION $e" "$out"
		fi
	done

	out=$(q "SELECT gp.version();")
	case "$out" in
		"PostgreSQL"*"Apache Cloudberry"*) ok "gp.version() -> $out" ;;
		*) notok "gp.version()" "$out" ;;
	esac

	out=$(q "DROP EXTENSION gp_sql;")
	if [ "$(q "SELECT count(*) FROM pg_extension WHERE extname = 'gp_sql';")" = "0" ]; then
		ok "DROP EXTENSION gp_sql"
	else
		notok "DROP EXTENSION gp_sql" "$out"
	fi

	###########################################################################
	echo "6. this node knows what it is, and its segment count is a divisor"
	###########################################################################
	# The segment count is not a flag.  A consumer divides by it -- ORCA
	# asserts 0 < segments when it builds its cost model and computes
	# 1.0 / segments in its skew model -- so it is never 0, and Cloudberry's
	# own getgpsegmentCount() answers 1 for a singleton for that reason.
	# Whether this server has segments configured at all is single_node.
	#
	# This test exists because the count was 0 and nothing could see it.
	out=$(q "SELECT segments FROM gp.node();")
	[ "$out" -ge 1 ] 2>/dev/null \
		&& ok "gp.node() reports at least one segment ($out)" \
		|| notok "segment count must be >= 1, it is a divisor" "$out"

	out=$(q "SELECT single_node FROM gp.node();")
	[ "$out" = "t" ] && ok "and says it is a single node" \
		|| notok "single_node on a server with no segments" "$out"

	out=$(q "SELECT role || ' ' || content_id FROM gp.node();")
	[ "$out" = "utility -1" ] && ok "as the coordinator, in utility role" \
		|| notok "role and content id" "$out"
else
	notok "server starts with all modules preloaded" "$(tail -20 "$PGDATA_ROOT/log")"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
