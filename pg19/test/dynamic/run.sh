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
# Dynamic tables: a materialized view that refreshes on a schedule.
#
# Cloudberry writes CREATE DYNAMIC TABLE ... SCHEDULE '...', which needs a
# keyword, a pg_class column and a statement of its own.  Here it is a
# materialized view with an option, a label and a gp_task job -- which is what
# Cloudberry's own code makes it, underneath.
#
# This suite waits for real time to pass: a schedule is in minutes.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/dynamic/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-dyn-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbd-XXXXXX)"
PORT="${PGPORT:-$((6500 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK"

pass=0; fail=0
ok()    { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok() { printf '  NOT OK %s\n' "$1"
          [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/         /'
          fail=$((fail + 1)); }

cleanup() {
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

q()  { "$PSQL" -X -q -t -A -d postgres -c "$1" 2>&1; }
qd() { "$PSQL" -X -q -t -A -d "$1" -c "$2" 2>&1; }

is() {
	local got; got=$(q "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

# isl <label> <sql> <expected>
#		The same, for a script whose answer is its last line.
isl() {
	local got; got=$(q "$2" | grep -v '^$' | tail -1)
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

refused() {
	local got; got=$(q "$2")
	case "$got" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$3], got [$got]" ;;
	esac
}

# eventually <label> <sql> <expected> [seconds]
eventually() {
	local deadline=$(( $(date +%s) + ${4:-100} )) got=
	while [ "$(date +%s)" -lt "$deadline" ]; do
		got=$(q "$2")
		[ "$got" = "$3" ] && { ok "$1"; return; }
		sleep 2
	done
	notok "$1" "want [$3], last saw [$got] after ${4:-100}s"
}

echo "dynamic tables: a materialized view on a schedule"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	# gp_matview reads CREATE MATERIALIZED VIEW before PostgreSQL does, and
	# gp_task's launcher is a background worker; both have to be preloaded.
	echo "shared_preload_libraries = 'gp_core,gp_task,gp_matview'"
	echo "gp.task_log_run = on"
	echo "max_worker_processes = 16"
} >> "$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_task CASCADE; CREATE EXTENSION gp_matview CASCADE;")
case "$out" in
	*ERROR*) echo "the extensions could not be created:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. a dynamic table is a materialized view, a label and a job"
###############################################################################
q "CREATE TABLE base (id int, amt numeric);
   INSERT INTO base VALUES (1, 10);
   CREATE MATERIALIZED VIEW dt WITH (gp.dynamic_schedule = '0 3 * * *') AS
     SELECT count(*) AS n, sum(amt) AS total FROM base;" > /dev/null

is "it is an ordinary materialized view" \
   "SELECT relkind FROM pg_class WHERE relname = 'dt';" "m"
is "and it holds what its query said when it was made" \
   "SELECT n || '/' || total FROM dt;" "1/10"
is "the schedule is readable, which is what says it is dynamic" \
   "SELECT gp_matview.dynamic_schedule('dt'::regclass);" "0 3 * * *"
is "an ordinary materialized view has none" \
   "CREATE MATERIALIZED VIEW plain AS SELECT 1 AS x;
    SELECT gp_matview.dynamic_schedule('plain'::regclass) IS NULL;" "t"
is "it is listed as a dynamic table" \
   "SELECT matviewname || ' | ' || schedule FROM gp_matview.dynamic_tables;" \
   "dt | 0 3 * * *"
is "and it has a refresh job, named as Cloudberry names it" \
   "SELECT command FROM gp_task.job
     WHERE jobname = 'gp_dynamic_table_refresh_' || 'dt'::regclass::oid;" \
   "REFRESH MATERIALIZED VIEW public.dt"

###############################################################################
echo "2. the schedule is optional, and is read before the view is made"
###############################################################################
q "CREATE MATERIALIZED VIEW dt_default WITH (gp.dynamic_schedule) AS SELECT 1 AS x;" > /dev/null
is "left out, it is the same default Cloudberry uses" \
   "SELECT gp_matview.dynamic_schedule('dt_default'::regclass);" "*/5 * * * *"
refused "a schedule nothing can run is refused" \
        "CREATE MATERIALIZED VIEW dt_bad WITH (gp.dynamic_schedule = 'every friday') AS SELECT 1;" \
        "is not a schedule"
is "and the view it would have made is not there either" \
   "SELECT count(*) FROM pg_class WHERE relname = 'dt_bad';" "0"
is "nor is a job for it" \
   "SELECT count(*) FROM gp_task.job WHERE command LIKE '%dt_bad%';" "0"

###############################################################################
echo "3. the job is an ordinary job, and the view an ordinary view"
###############################################################################
is "REFRESH works as it does on any materialized view" \
   "INSERT INTO base VALUES (2, 5);
    REFRESH MATERIALIZED VIEW dt;
    SELECT n || '/' || total FROM dt;" "2/15"
isl "the job's schedule can be changed with the scheduler's own function" \
   "CALL gp_task.alter_task('gp_dynamic_table_refresh_' || 'dt'::regclass::oid,
                            schedule => '0 4 * * *');
    SELECT schedule FROM gp_task.job
      WHERE jobname = 'gp_dynamic_table_refresh_' || 'dt'::regclass::oid;" "0 4 * * *"

###############################################################################
echo "4. dropping the view takes its job with it"
###############################################################################
q "DROP MATERIALIZED VIEW dt_default;" > /dev/null
is "the job of a dropped dynamic table is gone" \
   "SELECT count(*) FROM gp_task.job WHERE jobname LIKE 'gp_dynamic_table_refresh_%';" "1"
is "dropping a view that was never dynamic is still fine" \
   "DROP MATERIALIZED VIEW plain; SELECT count(*) FROM pg_class WHERE relname = 'plain';" "0"

###############################################################################
echo "5. it refreshes itself"
###############################################################################
q "CREATE TABLE ticker (n int);
   INSERT INTO ticker VALUES (1);
   CREATE MATERIALIZED VIEW dt_live WITH (gp.dynamic_schedule = '* * * * *') AS
     SELECT count(*) AS rows FROM ticker;" > /dev/null
is "it starts out with what the base table then held" \
   "SELECT rows FROM dt_live;" "1"
q "INSERT INTO ticker VALUES (2), (3);" > /dev/null
is "which does not follow the base table on its own" \
   "SELECT rows FROM dt_live;" "1"
eventually "until its schedule comes round" "SELECT rows FROM dt_live;" "3"
is "and the scheduler recorded the refresh" \
   "SELECT status FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'gp_dynamic_table_refresh_' || 'dt_live'::regclass::oid
     ORDER BY runid DESC LIMIT 1;" "succeeded"

###############################################################################
echo "6. what cannot work yet is refused, not written"
###############################################################################
# The jobs live in one database, because an extension cannot make a shared
# catalog.  A job written anywhere else is one the scheduler never reads.
q "CREATE DATABASE elsewhere;" > /dev/null
out=$(qd elsewhere "CREATE EXTENSION gp_matview CASCADE;
                    CREATE MATERIALIZED VIEW dt_there WITH (gp.dynamic_schedule) AS SELECT 1;")
case "$out" in
	*"cannot be made in database"*) ok "a dynamic table outside the task database is refused" ;;
	*) notok "a dynamic table outside the task database is refused" "$out" ;;
esac
case "$out" in
	*"gp.task_database"*) ok "and the message names the setting that decides where" ;;
	*) notok "and the message names the setting that decides where" "$out" ;;
esac
is "an ordinary materialized view there is unaffected" \
   "SELECT 1;" "1"
out=$(qd elsewhere "CREATE MATERIALIZED VIEW plain_there AS SELECT 1 AS x;
                    SELECT count(*) FROM pg_class WHERE relname = 'plain_there';")
[ "$(printf '%s' "$out" | tail -1)" = "1" ] \
	&& ok "and so is one made without the option" \
	|| notok "and so is one made without the option" "$out"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
