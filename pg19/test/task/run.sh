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
# This suite waits for real time to pass: once for a minute, to see a
# schedule of cron's run when its minute comes, and otherwise for seconds,
# with schedules of seconds, which Cloudberry's scheduler takes as well.
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
		sleep 0.5
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
echo "2. a schedule is cron's, read by Cloudberry's own parser, or seconds"
###############################################################################
accepted "five fields" "SELECT gp_task.validate_schedule('*/5 * * * *');"
accepted "names, not just numbers" "SELECT gp_task.validate_schedule('0 3 * * mon');"
accepted "a macro" "SELECT gp_task.validate_schedule('@daily');"
refused "a field too few" "SELECT gp_task.validate_schedule('* * * *');" "invalid schedule: * * * *"
refused "a minute that does not exist" "SELECT gp_task.validate_schedule('99 * * * *');" "invalid schedule"
refused "words that are not a schedule" "SELECT gp_task.validate_schedule('every tuesday');" "invalid schedule"
refused "refused in Cloudberry's words" "SELECT gp_task.validate_schedule('every tuesday');" \
        "HINT:  Use cron format (e.g. 5 4 * * *), or interval format '[1-59] seconds'"
# Cloudberry's own test's (src/test/regress/sql/task.sql), each way.
accepted "a number of seconds" "SELECT gp_task.validate_schedule('30 seconds');"
accepted "one second, in the singular" "SELECT gp_task.validate_schedule('1 second');"
accepted "in any case, with spaces around" "SELECT gp_task.validate_schedule(' 30 sEcOnDs ');"
accepted "and between" "SELECT gp_task.validate_schedule('17  seconds ');"
accepted "up to 59" "SELECT gp_task.validate_schedule('59 seconds');"
refused "not none" "SELECT gp_task.validate_schedule('0 seconds');" "invalid schedule: 0 seconds"
refused "nor a minute's worth" "SELECT gp_task.validate_schedule('60 seconds');" "invalid schedule: 60 seconds"
refused "nor fewer than none" "SELECT gp_task.validate_schedule('-1 seconds');" "invalid schedule: -1 seconds"
refused "nor more than a number holds" "SELECT gp_task.validate_schedule('1000000000000 seconds');" \
        "invalid schedule: 1000000000000 seconds"
refused "nor a word that is not seconds" "SELECT gp_task.validate_schedule('5 secondc');" "invalid schedule: 5 secondc"
refused "nor anything after it" "SELECT gp_task.validate_schedule('50 seconds c');" "invalid schedule: 50 seconds c"

###############################################################################
echo "3. tasks are written, changed and removed"
###############################################################################
is "creating one gives it an id" \
   "CALL gp_task.create_task('t1', '@daily', 'SELECT 1');
    SELECT jobid > 0 FROM gp_task.job WHERE jobname = 't1';" "t"
is "and a row that says what it is" \
   "SELECT schedule || ' | ' || command || ' | ' || active FROM gp_task.job WHERE jobname = 't1';" \
   "@daily | SELECT 1 | true"
is "it defaults to this database and this user" \
   "SELECT database = current_database() AND username = current_user FROM gp_task.job WHERE jobname = 't1';" "t"
refused "a task whose schedule cannot be read is refused when it is written" \
        "CALL gp_task.create_task('bad', 'not a schedule', 'SELECT 1');" "invalid schedule"
is "and leaves nothing behind" \
   "SELECT count(*) FROM gp_task.job WHERE jobname = 'bad';" "0"

accepted "altering one changes what it is given" \
         "CALL gp_task.alter_task('t1', schedule => '@hourly');"
is "and leaves what it is not" \
   "SELECT schedule || ' | ' || command FROM gp_task.job WHERE jobname = 't1';" \
   "@hourly | SELECT 1"
accepted "switching one off is an alteration like any other" \
         "CALL gp_task.alter_task('t1', active => false);"
is "and the row says so" \
   "SELECT active FROM gp_task.job WHERE jobname = 't1';" "f"
refused "altering one that is not there says so" \
        "CALL gp_task.alter_task('nosuch', schedule => '@daily');" "does not exist"
refused "and so does dropping it" "CALL gp_task.drop_task('{nosuch}');" "does not exist"
accepted "unless it is allowed to be missing" \
         "CALL gp_task.drop_task('{nosuch}', missing_ok => true);"
accepted "dropping one removes it" \
         "CALL gp_task.drop_task('{t1}');"
is "and it is gone" "SELECT count(*) FROM gp_task.job WHERE jobname = 't1';" "0"

###############################################################################
echo "4. a task runs, and its history says so"
###############################################################################
q "CREATE TABLE ran (at timestamptz DEFAULT now());
   CALL gp_task.create_task('every_minute', '* * * * *', 'INSERT INTO ran DEFAULT VALUES');" > /dev/null

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
q "CALL gp_task.create_task('breaks', '1 second', 'SELECT 1 / 0');" > /dev/null
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
q "CALL gp_task.create_task('over_there', '1 second',
       'INSERT INTO elsewhere DEFAULT VALUES', database => 'other_db');" > /dev/null

deadline=$(( $(date +%s) + 100 )); got=
while [ "$(date +%s)" -lt "$deadline" ]; do
	got=$(qd other_db "SELECT count(*) > 0 FROM elsewhere;")
	[ "$got" = "t" ] && break
	sleep 0.5
done
[ "$got" = "t" ] && ok "the command runs in the database it names" \
                 || notok "the command runs in the database it names" "last saw [$got]"
eventually "and its history is still in the database the scheduler reads" \
   "SELECT count(*) > 0 FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'over_there' AND h.status = 'succeeded';" "t" 30

###############################################################################
echo "7. a task that names something that is not there fails cleanly"
###############################################################################
q "CALL gp_task.create_task('nowhere', '1 second', 'SELECT 1', database => 'no_such_db');" > /dev/null
eventually "a missing database is reported against the job, not the server" \
           "SELECT return_message FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
              WHERE j.jobname = 'nowhere' ORDER BY runid DESC LIMIT 1;" \
           "database \"no_such_db\" does not exist"
is "the server is still up" "SELECT 1;" "1"

###############################################################################
echo "8. dropping a task takes its history with it, and stops it running"
###############################################################################
q "CALL gp_task.drop_task('{nowhere,breaks}');" > /dev/null
is "the history of a dropped task is gone" \
   "SELECT count(*) FROM gp_task.run_history h
     WHERE NOT EXISTS (SELECT 1 FROM gp_task.job j WHERE j.jobid = h.jobid);" "0"

q "CALL gp_task.alter_task('every_minute', active => false);
   CREATE TABLE ticks (at timestamptz DEFAULT now());
   CALL gp_task.create_task('every_second', '1 second', 'INSERT INTO ticks DEFAULT VALUES');" > /dev/null
eventually "a task of one second runs" "SELECT count(*) >= 2 FROM ticks;" "t" 30
q "CALL gp_task.alter_task('every_second', active => false);" > /dev/null
# A run it began before it was switched off may end after; then its seconds
# pass three times over.
sleep 2
q "DELETE FROM ticks;" > /dev/null
sleep 3
is "a task switched off does not run" "SELECT count(*) FROM ticks;" "0"
is "though it is still there" \
   "SELECT active FROM gp_task.job WHERE jobname = 'every_second';" "f"

###############################################################################
echo "9. a task written in another database is written in the scheduler's"
###############################################################################
# Through gp_core's loopback, as the writing transaction commits: what a
# rolled-back statement or savepoint asked for is never written, and what
# the task database says of it -- its errors, its notices -- is said then.
qd other_db "CREATE EXTENSION gp_task CASCADE;" > /dev/null
out=$(qd other_db "CALL gp_task.create_task('from_other', '@daily', 'SELECT 1');")
is "a task created in another database is a job here" \
   "SELECT database || ' ' || username = 'other_db ' || current_user
      FROM gp_task.job WHERE jobname = 'from_other';" "t"
out=$(qd other_db "SELECT count(*) FROM gp_task.job;")
[ "$out" = "0" ] && ok "and not a row in that database's own table" \
	|| notok "and not a row in that database's own table" "$out"
qd other_db "BEGIN; CALL gp_task.create_task('rolled_back', '@daily', 'SELECT 1'); ROLLBACK;" > /dev/null
is "one whose transaction rolled back is not written" \
   "SELECT count(*) FROM gp_task.job WHERE jobname = 'rolled_back';" "0"
qd other_db "BEGIN;
             SAVEPOINT s; CALL gp_task.create_task('undone', '@daily', 'SELECT 1'); ROLLBACK TO s;
             CALL gp_task.create_task('done', '@daily', 'SELECT 1');
             COMMIT;" > /dev/null
is "nor is one whose savepoint rolled back, beside one that did not" \
   "SELECT string_agg(jobname, ',' ORDER BY jobname) FROM gp_task.job
     WHERE jobname IN ('undone', 'done');" "done"
out=$(qd other_db "CALL gp_task.alter_task('from_other', schedule => '@hourly');
                   CALL gp_task.drop_task('{done}');")
is "it is altered and dropped from there too" \
   "SELECT string_agg(jobname || ' ' || schedule, ',') FROM gp_task.job
     WHERE jobname IN ('from_other', 'done');" "from_other @hourly"
out=$(qd other_db "CALL gp_task.drop_task('{nosuch}');")
case "$out" in
	*'task "nosuch" does not exist'*'run in database "postgres" as the transaction commits'*)
		ok "the task database's refusal comes back as the transaction commits, and says so" ;;
	*) notok "the task database's refusal comes back as the transaction commits, and says so" "$out" ;;
esac
out=$(qd other_db "CALL gp_task.create_task('from_other', '@daily', 'SELECT 1', if_not_exists => true);")
case "$out" in
	*'task "from_other" already exists, skipping'*) ok "and so does its notice" ;;
	*) notok "and so does its notice" "$out" ;;
esac
out=$(qd other_db "CALL gp_task.create_task('bad_there', 'not a schedule', 'SELECT 1');")
case "$out" in
	*"invalid schedule"*) ok "a schedule nothing can run is refused there at once" ;;
	*) notok "a schedule nothing can run is refused there at once" "$out" ;;
esac
out=$(qd other_db "BEGIN READ ONLY;
                   CALL gp_task.create_task('read_only', '@daily', 'SELECT 1');
                   COMMIT;")
case "$out" in
	*"read-only transaction"*) ok "and a read-only transaction writes nothing there" ;;
	*) notok "and a read-only transaction writes nothing there" "$out" ;;
esac

###############################################################################
echo "10. a task of seconds runs as Cloudberry's does: an interval after it is written, one run at a time"
###############################################################################
# The first run comes one interval after the scheduler reads the job, which
# is after it was written; a run that outlasts its interval is owed one more,
# never run beside it.
q "CREATE TABLE made (at timestamptz);
   INSERT INTO made VALUES (clock_timestamp());
   CALL gp_task.create_task('five_seconds', '5 seconds', 'SELECT 1');
   CALL gp_task.create_task('slow', '1 second', 'SELECT pg_sleep(2)');" > /dev/null
eventually "a task of five seconds runs" \
   "SELECT count(*) > 0 FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'five_seconds';" "t" 30
is "and not before five seconds had passed since it was written" \
   "SELECT bool_and(h.start_time >= m.at + interval '5 seconds')
      FROM gp_task.run_history h JOIN gp_task.job j USING (jobid), made m
     WHERE j.jobname = 'five_seconds';" "t"
eventually "a task whose runs outlast its second runs again and again" \
   "SELECT count(*) >= 3 FROM gp_task.run_history h JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'slow' AND h.status = 'succeeded';" "t" 30
is "but never beside itself" \
   "SELECT count(*) FROM gp_task.run_history a JOIN gp_task.run_history b USING (jobid)
                        JOIN gp_task.job j USING (jobid)
     WHERE j.jobname = 'slow' AND a.runid < b.runid
       AND b.start_time < coalesce(a.end_time, 'infinity');" "0"
q "CALL gp_task.drop_task('{five_seconds,slow,every_second}');" > /dev/null

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
