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
# gp_task: the task scheduler.
#
# Cloudberry writes CREATE TASK and keeps the jobs in shared catalogs; here
# they are functions over tables in one database, which is pg_cron's shape and
# the one "Cluster metadata without shared catalogs" leaves available.
#
# This suite waits for real time to pass: a schedule is in minutes, so a task
# that runs cannot be observed any faster than a minute.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/task/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-task-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbt-XXXXXX)"
PORT="${PGPORT:-$((6300 + RANDOM % 200))}"
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

# is <label> <sql> <expected>
is() {
	local got; got=$(q "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

# accepted <label> <sql>
#		For the functions that answer with nothing: a void result is not NULL,
#		it is an empty string, so what is asserted is that nothing was raised.
accepted() {
	local got; got=$(q "$2")
	case "$got" in
		*ERROR*) notok "$1" "$got" ;;
		*) ok "$1" ;;
	esac
}

# refused <label> <sql> <text the error must contain>
refused() {
	local got; got=$(q "$2")
	case "$got" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$3], got [$got]" ;;
	esac
}

# eventually <label> <sql> <expected> [seconds]
#		A scheduler is watched rather than asked, so the answer is the first
#		one that arrives before the deadline.
eventually() {
	local deadline=$(( $(date +%s) + ${4:-100} )) got=
	while [ "$(date +%s)" -lt "$deadline" ]; do
		got=$(q "$2")
		[ "$got" = "$3" ] && { ok "$1"; return; }
		sleep 2
	done
	notok "$1" "want [$3], last saw [$got] after ${4:-100}s"
}

echo "gp_task: the task scheduler"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	# The scheduler is a background worker, so the module is preloaded.
	echo "shared_preload_libraries = 'gp_core,gp_task'"
	echo "gp.task_log_run = on"
	# Each running job holds one; the launcher itself holds another.
	echo "max_worker_processes = 16"
} >> "$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_task CASCADE;")
case "$out" in
	*ERROR*) echo "CREATE EXTENSION gp_task failed, so nothing below is worth running:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. the scheduler is running, and its tables are where it looks"
###############################################################################
is "the launcher is a background worker of its own" \
   "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'gp_task scheduler';" "1"
is "it says which database it reads its jobs from" \
   "SELECT count(*) > 0 FROM pg_settings WHERE name = 'gp.task_database' AND setting = 'postgres';" "t"
is "the job table is there" \
   "SELECT count(*) FROM pg_tables WHERE schemaname = 'gp_task' AND tablename = 'job';" "1"
is "and so is the history" \
   "SELECT count(*) FROM pg_tables WHERE schemaname = 'gp_task' AND tablename = 'run_history';" "1"
is "neither is readable by everybody, because they hold other people's commands" \
   "SELECT count(*) FROM pg_tables WHERE schemaname = 'gp_task'
      AND pg_catalog.has_table_privilege('public', schemaname || '.' || tablename, 'SELECT');" "0"

###############################################################################
echo "2. a schedule is read by Cloudberry's own cron parser"
###############################################################################
accepted "five fields" "SELECT gp_task.validate_schedule('*/5 * * * *');"
accepted "names, not just numbers" "SELECT gp_task.validate_schedule('0 3 * * mon');"
accepted "a macro" "SELECT gp_task.validate_schedule('@daily');"
refused "a field too few" "SELECT gp_task.validate_schedule('* * * *');" "is not a schedule"
refused "a minute that does not exist" "SELECT gp_task.validate_schedule('99 * * * *');" "is not a schedule"
refused "words that are not a schedule" "SELECT gp_task.validate_schedule('every tuesday');" "is not a schedule"

###############################################################################
echo "3. tasks are written, changed and removed"
###############################################################################
is "creating one gives it an id" \
   "SELECT gp_task.create_task('t1', '@daily', 'SELECT 1') > 0;" "t"
is "and a row that says what it is" \
   "SELECT schedule || ' | ' || command || ' | ' || active FROM gp_task.job WHERE jobname = 't1';" \
   "@daily | SELECT 1 | true"
is "it defaults to this database and this user" \
   "SELECT database = current_database() AND username = current_user FROM gp_task.job WHERE jobname = 't1';" "t"
refused "a task whose schedule cannot be read is refused when it is written" \
        "SELECT gp_task.create_task('bad', 'not a schedule', 'SELECT 1');" "is not a schedule"
is "and leaves nothing behind" \
   "SELECT count(*) FROM gp_task.job WHERE jobname = 'bad';" "0"

accepted "altering one changes what it is given" \
         "SELECT gp_task.alter_task('t1', schedule => '@hourly');"
is "and leaves what it is not" \
   "SELECT schedule || ' | ' || command FROM gp_task.job WHERE jobname = 't1';" \
   "@hourly | SELECT 1"
accepted "switching one off is an alteration like any other" \
         "SELECT gp_task.alter_task('t1', active => false);"
is "and the row says so" \
   "SELECT active FROM gp_task.job WHERE jobname = 't1';" "f"
refused "altering one that is not there says so" \
        "SELECT gp_task.alter_task('nosuch', schedule => '@daily');" "does not exist"
refused "and so does dropping it" "SELECT gp_task.drop_task('nosuch');" "does not exist"
accepted "unless it is allowed to be missing" \
         "SELECT gp_task.drop_task('nosuch', missing_ok => true);"
accepted "dropping one removes it" \
         "SELECT gp_task.drop_task('t1');"
is "and it is gone" "SELECT count(*) FROM gp_task.job WHERE jobname = 't1';" "0"

###############################################################################
echo "4. a task runs, and its history says so"
###############################################################################
q "CREATE TABLE ran (at timestamptz DEFAULT now());
   SELECT gp_task.create_task('every_minute', '* * * * *', 'INSERT INTO ran DEFAULT VALUES');" > /dev/null

eventually "the command runs when its minute comes" \
           "SELECT count(*) > 0 FROM ran;" "t"
eventually "and the history says it succeeded" \
   "SELECT status FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'every_minute' ORDER BY runid DESC LIMIT 1;" "succeeded" 30
eventually "with the times it ran between" \
   "SELECT start_time IS NOT NULL AND end_time IS NOT NULL
      FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'every_minute' ORDER BY runid DESC LIMIT 1;" "t" 30
eventually "and the process that ran it" \
   "SELECT job_pid IS NOT NULL AND job_pid <> 0
      FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'every_minute' ORDER BY runid DESC LIMIT 1;" "t" 30

###############################################################################
echo "5. a task that fails is recorded as failing, with its reason"
###############################################################################
q "SELECT gp_task.create_task('breaks', '* * * * *', 'SELECT 1 / 0');" > /dev/null
eventually "a failing command is recorded" \
           "SELECT status FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
              WHERE j.jobname = 'breaks' ORDER BY runid DESC LIMIT 1;" "failed"
eventually "and the message says why" \
   "SELECT return_message FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'breaks' ORDER BY runid DESC LIMIT 1;" "division by zero" 30
eventually "a failing job does not stop the one beside it" \
   "SELECT count(*) > 0 FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'every_minute' AND h.status = 'succeeded';" "t" 30

###############################################################################
echo "6. a task names the database it runs in, which need not be this one"
###############################################################################
q "CREATE DATABASE other_db;" > /dev/null
qd other_db "CREATE TABLE elsewhere (at timestamptz DEFAULT now());" > /dev/null
q "SELECT gp_task.create_task('over_there', '* * * * *',
       'INSERT INTO elsewhere DEFAULT VALUES', database => 'other_db');" > /dev/null

deadline=$(( $(date +%s) + 100 )); got=
while [ "$(date +%s)" -lt "$deadline" ]; do
	got=$(qd other_db "SELECT count(*) > 0 FROM elsewhere;")
	[ "$got" = "t" ] && break
	sleep 2
done
[ "$got" = "t" ] && ok "the command runs in the database it names" \
                 || notok "the command runs in the database it names" "last saw [$got]"
eventually "and its history is still in the database the scheduler reads" \
   "SELECT count(*) > 0 FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'over_there' AND h.status = 'succeeded';" "t" 30

###############################################################################
echo "7. a task that names something that is not there fails cleanly"
###############################################################################
q "SELECT gp_task.create_task('nowhere', '* * * * *', 'SELECT 1', database => 'no_such_db');" > /dev/null
eventually "a missing database is reported against the job, not the server" \
           "SELECT return_message FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
              WHERE j.jobname = 'nowhere' ORDER BY runid DESC LIMIT 1;" \
           "database \"no_such_db\" does not exist"
is "the server is still up" "SELECT 1;" "1"

###############################################################################
echo "8. dropping a task takes its history with it, and stops it running"
###############################################################################
q "SELECT gp_task.drop_task('nowhere'); SELECT gp_task.drop_task('breaks');" > /dev/null
is "the history of a dropped task is gone" \
   "SELECT count(*) FROM gp_task.run_history h
     WHERE NOT EXISTS (SELECT 1 FROM gp_task.job j WHERE j.jobid = h.jobid);" "0"

q "SELECT gp_task.alter_task('every_minute', active => false);
   DELETE FROM ran;" > /dev/null
sleep 70
is "a task switched off does not run" "SELECT count(*) FROM ran;" "0"
is "though it is still there" \
   "SELECT active FROM gp_task.job WHERE jobname = 'every_minute';" "f"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
