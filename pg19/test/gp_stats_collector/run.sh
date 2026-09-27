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
# Cloudberry's gp_stats_collector tests, on a cluster and on one node: M8's.
#
# gpcontrib/gp_stats_collector's installcheck is what Cloudberry's CI runs
# against its demo cluster -- a coordinator and three segments -- with the
# library preloaded on every node (gpcontrib-gp-stats-collector,
# .github/workflows/build-cloudberry.yml): its eight tests, which read what
# the collector wrote into its log table, gpsc.log, every node's, and one,
# gpsc_uds, which reads what it sent to a Unix socket the test listens on
# itself.  The port runs them on a coordinator and three segments of its own,
# with the port's modules and gp_stats_collector preloaded after them, and on
# one node -- all but gpsc_dist, which is about the segments -- in the
# database pg_regress makes, which has gp_core's and gp_sql's extensions.
# A setting of Cloudberry's core is respelled as the port spells it, as the
# greenplum suite respells it, and gpsc_uds's socket is one of the run's
# own, so that two runs on one host do not take each other's.
#
# Each test is compared as Cloudberry's pg_regress compares it: gpdiff.pl,
# under Cloudberry's init_file and the port's greenplum one, or exactly a
# difference in cloudberry/, reviewed and kept -- <test>.diff on the cluster,
# <test>.single.diff on one node, and either with .planner or .orca before
# .diff for one pass alone (see ../singlenode/canon.pl for the form).  Two
# passes, the planner (gp.optimizer = off) and ORCA, as PASSES says.

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GPSC="${CB_GPSC_DIR:-/cb/gpcontrib/gp_stats_collector}"
CB="${CB_REGRESS_DIR:-/cb/src/test/regress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"
# the Makefile's REGRESS, in its order
TESTS="gpsc_cursors gpsc_dist gpsc_select gpsc_utf8_trim gpsc_utility gpsc_guc_cache gpsc_uds gpsc_locale"
SINGLE_TESTS="gpsc_cursors gpsc_select gpsc_utf8_trim gpsc_utility gpsc_guc_cache gpsc_uds gpsc_locale"

if [ ! -d "$GPSC/sql" ] || [ ! -x "$PG_REGRESS" ] || [ ! -f "$CB/gpdiff.pl" ] ||
   [ ! -f "$("$BINDIR/pg_config" --sharedir)/extension/gp_stats_collector.control" ]; then
	echo "gp_stats_collector's tests, pg_regress, gpdiff.pl or gp_stats_collector is not installed; skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-gpsc-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbgs-XXXXXX)"
# What is executed cannot be in /tmp, which the Compose project mounts noexec.
EXEC="$(mktemp -d "${HOME:-/var/lib/postgresql}/cb-gs-XXXXXX")"
BASEPORT="${PGPORT:-$((7100 + RANDOM % 200))}"
SECRET="gp-stats-collector-$RANDOM$RANDOM$RANDOM"
NODES="0 1 2 3"			# a coordinator and Cloudberry's three segments; 9 is the one node
# On the cluster, gp_resource too, whose resource queues Cloudberry's demo
# cluster has on, and after the collector, so that a query's wait in a
# queue comes first, before the collector's own hook has seen the query.
PRELOAD='gp_core,gp_orca,gp_sql,gp_stats_collector,gp_resource'
PRELOAD_ONE='gp_core,gp_orca,gp_sql,gp_stats_collector'

port() { echo $((BASEPORT + $1)); }
# The superuser is Cloudberry's demo cluster's, gpadmin: gpsc_guc_cache's
# expected output names it.
export PGUSER=gpadmin

cleanup() {
	for n in $NODES 9; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$WORK/node$n.log" "$RESULTS_DIR/gpsc-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$WORK/node$n" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK" "$EXEC"
}
trap cleanup EXIT

echo "gp_stats_collector: Cloudberry's gp_stats_collector tests, on a coordinator and three segments and on one node"
echo

# The nodes: the cluster, as the greenplum suite makes one, and the one node,
# each with gp_stats_collector preloaded after the port's modules.
CONF="$WORK/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $SOCK/n$n $(port "$n") $WORK/node$n"
done > "$CONF"
for n in $NODES 9; do
	mkdir -p "$SOCK/n$n"
	"$BINDIR/initdb" -D "$WORK/node$n" -N -U gpadmin --locale=C --encoding=UTF8 > "$WORK/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$WORK/initdb$n.log"; exit 1; }
	{
		echo "unix_socket_directories = '$SOCK/n$n'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "fsync = off"
		if [ "$n" != 9 ]; then
			echo "shared_preload_libraries = '$PRELOAD'"
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			echo "max_prepared_transactions = 64"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		else
			echo "shared_preload_libraries = '$PRELOAD_ONE'"
		fi
	} >> "$WORK/node$n/postgresql.auto.conf"
done
for n in 1 2 3 0 9; do
	"$BINDIR/pg_ctl" -D "$WORK/node$n" -l "$WORK/node$n.log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "node $n did not start"; tail -20 "$WORK/node$n.log"; exit 1; }
done
# gp_core's extension in the one node's template0 too, from which gpsc_locale
# makes a database: a cluster's coordinator makes it in every new database,
# as Cloudberry's initdb has its core's objects in every template, and the
# collector's script reads them (gp_dist_random()).
"$PSQL" -X -q -h "$SOCK/n9" -p "$(port 9)" -d postgres \
	-c "ALTER DATABASE template0 ALLOW_CONNECTIONS true" &&
"$PSQL" -X -q -h "$SOCK/n9" -p "$(port 9)" -d template0 -c "CREATE EXTENSION gp_core" &&
"$PSQL" -X -q -h "$SOCK/n9" -p "$(port 9)" -d postgres \
	-c "ALTER DATABASE template0 ALLOW_CONNECTIONS false" ||
	{ echo "could not make gp_core's extension in the one node's template0"; exit 1; }

# The settings the port has, and what respells Cloudberry's names for them,
# as the greenplum suite makes it (../respell.pl).
{
	echo "kinds field set func header"
	PGOPTIONS="-c gp.optimizer=off" "$PSQL" -X -q -t -A -h "$SOCK/n0" -p "$(port 0)" -d postgres \
		-c "SELECT name FROM pg_settings WHERE name LIKE 'gp.%' ORDER BY length(name) DESC" |
	while read -r name; do
		short="${name#gp.}"
		case "$short" in
			optimizer*|statement_mem|enable_parallel|enable_groupagg|test_print_*|\
			resource_scheduler|resource_select_only|resource_cleanup_gangs_on_wait|\
			max_resource_queues|max_resource_portals_per_transaction|max_statement_mem|\
			debug_resource_group|runaway_detector_activation_percent|\
			vmem_process_interrupt|explain_memory_verbosity|coredump_on_memerror|\
			enable_offload_entry_to_qe|debug_dtm_action*|debug_abort_after_distributed_prepared|\
			debug_print_full_dtm|enable_answer_query_using_materialized_views|aqumv_allow_foreign_table)
				cbname="$short" ;;
			*) cbname="gp_$short" ;;
		esac
		echo "map $cbname $name"
	done
	echo "sed s#'/tmp/gpsc_test\\.sock'#'$SOCK/gpsc_test.sock'#g"
} > "$WORK/respell"
respell() { perl "$HERE/../respell.pl" "$WORK/respell" "$@"; }

SN="$WORK/cases"
mkdir -p "$SN/sql" "$SN/expected"
for t in $TESTS; do
	respell "$GPSC/sql/$t.sql" > "$SN/sql/$t.sql"
	respell "$GPSC/expected/$t.out" > "$SN/expected/$t.out"
done

mkdir -p "$WORK/gpdiff"
cp "$CB"/gpdiff.pl "$CB"/atmsort.pm "$CB"/explain.pm "$WORK/gpdiff/"
sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
	"$CB/GPTest.pm.in" > "$WORK/gpdiff/GPTest.pm"

# The diff pg_regress runs: gpdiff.pl, with a difference reviewed and kept
# counted as none.  GS_WHERE is "" on the cluster and ".single" on the one
# node, GS_PASS the pass, which name a difference kept for them; one that
# depends on the run has a kept alternative for each way it may come out,
# <name>.<pass>.<n>.diff.
mkdir -p "$EXEC/bin"
cat > "$EXEC/bin/diff" <<EOF
#!/bin/bash
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)\$GS_WHERE
cb=(-I HINT: -I CONTEXT: -I GP_IGNORE: --gpd_ignore_plans
    --gpd_init "$CB/init_file" --gpd_init "$HERE/../greenplum/init_file")
canon="$WORK/canon/\$GS_PASS/\$name.diff"
mkdir -p "$WORK/canon/\$GS_PASS"
env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" -U0 "\${cb[@]}" "\$exp" "\$res" 2> /dev/null |
	perl "$HERE/../singlenode/canon.pl" > "\$canon"
st=("\${PIPESTATUS[@]}")
[ "\${st[0]}" -le 1 ] && [ "\${st[1]}" -eq 0 ] || echo "no comparison was made" >> "\$canon"
for kept in "$HERE/cloudberry/\$name.\$GS_PASS.diff" "$HERE/cloudberry/\$name.diff" \
	"$HERE/cloudberry/\$name.\$GS_PASS".[0-9].diff; do
	if [ ! -s "\$canon" ] || cmp -s "\$canon" "\$kept"; then
		rm -f "\$canon"
		exit 0
	fi
done
exec env PATH=/usr/bin:/bin perl "$WORK/gpdiff/gpdiff.pl" "\${opts[@]}" "\${cb[@]}" "\$exp" "\$res"
EOF
chmod +x "$EXEC/bin/diff"

# What the port adds: the events that end a query as it fails, which the
# tests do not ask for -- an error and a cancel, the query's own on the
# coordinator or its slice's on a segment, reported from the executor's
# hooks, and a query failed in its resource queue, which gp_resource tells
# of -- each with its message, and in the log table past the abort of the
# transaction that wrote it.  On the cluster and on the one node, after each
# pass's tests, in a database of the pass's own.
ok()   { printf '  ok     %s\n' "$1"; }
notok(){ printf '  NOT OK %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/         /'; rc=1; }
is() {						# is <name> <node> <sql> <want>
	local got
	got=$("$PSQL" -X -q -t -A -h "$SOCK/n$2" -p "$(port "$2")" -d "$DB" -c "$3" 2>&1)
	[ "$got" = "$4" ] && ok "$1" || notok "$1" "want [$4], got [$got]"
}
# the statements' events, which a session with the collector on runs
events() {					# events <node>: statements on stdin
	"$PSQL" -X -q -h "$SOCK/n$1" -p "$(port "$1")" -d "$DB" \
		-c "SET gpsc.enable = on" -c "SET gpsc.ignored_users_list = ''" \
		-c "SET gpsc.logging_mode = tbl" -f - > /dev/null 2>&1
}
status() {					# status <text>: the coordinator's events of the query
	echo "SELECT string_agg(query_status || coalesce(': ' || error_message, ''), ', '
	                        ORDER BY gpsc_status_order(query_status))
	        FROM gpsc.log WHERE segid = -1 AND query_text = '$1'"
}
checks() {					# checks <pass>
	local node holder
	DB="gpsc_checks_$1"
	for node in 0 9; do
		echo "== $1, the ends of a query's life, $([ "$node" = 9 ] && echo "one node" || echo "a coordinator and three segments")"
		"$PSQL" -X -q -h "$SOCK/n$node" -p "$(port "$node")" -d postgres \
			-c "CREATE DATABASE $DB" > /dev/null &&
		"$PSQL" -X -q -h "$SOCK/n$node" -p "$(port "$node")" -d "$DB" \
			-c "SET client_min_messages = warning" \
			-c "CREATE EXTENSION IF NOT EXISTS gp_core" -c "CREATE EXTENSION gp_sql" \
			-c "CREATE EXTENSION gp_stats_collector" -c "CREATE TABLE t (a int) DISTRIBUTED BY (a)" \
			-c "INSERT INTO t VALUES (0)" \
			-c "CREATE FUNCTION gpsc_status_order(s text) RETURNS int LANGUAGE sql IMMUTABLE
			      AS \$\$SELECT array_position(ARRAY['QUERY_STATUS_SUBMIT', 'QUERY_STATUS_START',
			        'QUERY_STATUS_END', 'QUERY_STATUS_DONE', 'QUERY_STATUS_ERROR',
			        'QUERY_STATUS_CANCELED'], s)\$\$" > /dev/null ||
			{ notok "the database $DB"; continue; }
		events "$node" <<-'EOF'
			SELECT 1/a FROM t;
			SET statement_timeout = '200ms';
			SELECT pg_sleep(5);
			RESET statement_timeout;
			BEGIN;
			SELECT 2;
			ROLLBACK;
			SELECT a FROM t WHERE 1/a > 0;
		EOF
		is "an error: the query's, from the executor's hook, with its message" "$node" "$(status 'SELECT 1/a FROM t;')" \
			"QUERY_STATUS_SUBMIT, QUERY_STATUS_START, QUERY_STATUS_ERROR: division by zero"
		is "a cancel, as cancelled" "$node" "$(status 'SELECT pg_sleep(5);')" \
			"QUERY_STATUS_SUBMIT, QUERY_STATUS_START, QUERY_STATUS_CANCELED"
		is "and the rows of a rolled back transaction are kept, written frozen" "$node" "$(status 'SELECT 2;')" \
			"QUERY_STATUS_SUBMIT, QUERY_STATUS_START, QUERY_STATUS_END, QUERY_STATUS_DONE"
		[ "$node" = 9 ] && continue
		# the condition a segment's scan checks, on either planner's route;
		# the segment's events under the coordinator's count of the statement
		is "a segment's error, the segment's slice's, which reports it under the statement's count" 0 \
			"SELECT count(*) FROM gpsc.log WHERE segid IS DISTINCT FROM -1 AND query_status = 'QUERY_STATUS_ERROR'
			   AND error_message = 'division by zero'
			   AND ccnt = (SELECT ccnt FROM gpsc.log WHERE segid = -1
			                  AND query_text = 'SELECT a FROM t WHERE 1/a > 0;' LIMIT 1)" 1

		# A queue of one statement, which a cursor of the role holds until
		# told: the role's next query waits in it, and is cancelled there.
		# The role's sessions are the collector's by the role's settings,
		# which only a superuser sets.
		"$PSQL" -X -q -d "$DB" -h "$SOCK/n0" -p "$(port 0)" \
			-c "SET client_min_messages = warning" \
			-c "CREATE EXTENSION gp_resource" \
			-c "CREATE RESOURCE QUEUE gpsc_q_$1 WITH (ACTIVE_STATEMENTS = 1)" \
			-c "CREATE ROLE gpsc_role_$1 LOGIN RESOURCE QUEUE gpsc_q_$1" \
			-c "GRANT SELECT ON t TO gpsc_role_$1" \
			-c "ALTER ROLE gpsc_role_$1 SET gpsc.enable = on" \
			-c "ALTER ROLE gpsc_role_$1 SET gpsc.ignored_users_list = ''" \
			-c "ALTER ROLE gpsc_role_$1 SET gpsc.logging_mode = tbl" > /dev/null ||
			{ notok "the resource queue gpsc_q_$1"; continue; }
		rm -f "$WORK/waited"
		printf '%s\n' "BEGIN;" "DECLARE c CURSOR FOR SELECT * FROM t;" \
			"\\! for i in \$(seq 300); do [ -e '$WORK/waited' ] && break; sleep 0.1; done" "COMMIT;" |
			PGUSER=gpsc_role_$1 "$PSQL" -X -q -h "$SOCK/n0" -p "$(port 0)" -d "$DB" -f - > "$WORK/holder.out" 2>&1 &
		holder=$!
		for i in $(seq 100); do
			[ "$("$PSQL" -X -q -t -A -h "$SOCK/n0" -p "$(port 0)" -d "$DB" \
				-c "SELECT rsqholders FROM pg_resqueue_status WHERE rsqname = 'gpsc_q_$1'")" = 1 ] && break
			sleep 0.1
		done
		printf '%s\n' "SET statement_timeout = '1s';" "SELECT count(*) FROM t;" |
			PGUSER=gpsc_role_$1 "$PSQL" -X -q -h "$SOCK/n0" -p "$(port 0)" -d "$DB" -f - > "$WORK/waiter.out" 2>&1
		touch "$WORK/waited"
		wait "$holder"
		is "a query that waits in its resource queue and is cancelled there, as gp_resource tells" 0 \
			"$(status 'SELECT count(*) FROM t;')" \
			"QUERY_STATUS_SUBMIT, QUERY_STATUS_ERROR: canceling statement due to statement timeout"
		grep -q ERROR "$WORK/holder.out" &&
			notok "and the queue's holder, a role that is no superuser, ran on the cluster" "$(cat "$WORK/holder.out")"
	done
}

rc=0
for pass in ${PASSES:-planner orca}; do
	[ "$pass" = orca ] && optimizer=on || optimizer=off
	for where in cluster single; do
		if [ "$where" = single ]; then node=9; tests=$SINGLE_TESTS; else node=0; tests=$TESTS; fi
		out="$WORK/out-$pass-$where"
		echo "== $pass, $([ "$where" = single ] && echo "one node" || echo "a coordinator and three segments")"
		GS_WHERE=$([ "$where" = single ] && echo .single) GS_PASS=$pass \
		PATH="$EXEC/bin:$PATH" \
		PGOPTIONS="-c gp.optimizer=$optimizer -c statement_timeout=${STATEMENT_TIMEOUT:-60s}" \
			"$PG_REGRESS" \
				--bindir="$BINDIR" \
				--inputdir="$SN" \
				--expecteddir="$SN" \
				--outputdir="$out" \
				--load-extension=gp_core \
				--load-extension=gp_sql \
				--dbname=contrib_regression \
				--host="$SOCK/n$node" --port="$(port "$node")" \
				$tests \
			> "$out.log" 2>&1
		[ $? -eq 0 ] || rc=1
		grep -E "^(not )?ok " "$out.log" | sed 's/^/  /'
		total=$(grep -cE "^(not )?ok " "$out.log")
		bad=$(grep -cE "^not ok " "$out.log")
		echo "  $((total - bad)) of $total passed"
		if [ "$total" -eq 0 ]; then
			tail -5 "$out.log" | sed 's/^/  /'
			rc=1
		fi
		if [ -n "${RESULTS_DIR:-}" ] && [ "$bad" -ne 0 ]; then
			mkdir -p "$RESULTS_DIR/gpsc-$pass-$where"
			cp "$out/regression.diffs" "$RESULTS_DIR/gpsc-$pass-$where.diffs" 2> /dev/null
			cp -r "$out"/results/. "$RESULTS_DIR/gpsc-$pass-$where/" 2> /dev/null
		fi
	done
	PGOPTIONS="-c gp.optimizer=$optimizer -c statement_timeout=${STATEMENT_TIMEOUT:-60s}" checks "$pass"
done


unreviewed=$(cd "$WORK/canon" 2> /dev/null && ls -- */*.diff 2> /dev/null | sed 's/\.diff$//')
if [ -n "$unreviewed" ]; then
	echo "  differences no one has reviewed, pass/test:"
	echo $unreviewed | fold -s -w 72 | sed 's/^/    /'
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/gpsc-canon"
		cp -r "$WORK"/canon/. "$RESULTS_DIR/gpsc-canon/"
	fi
fi

[ "$rc" -eq 0 ]
