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
# Password profiles, account locking and password rules.
#
# Cloudberry keeps all of this in two shared catalogs, seven pg_authid columns
# and a pair of postmaster children.  Here it is shared security labels, a
# table nobody may read, and one background worker -- so these tests log in
# for real, over a socket that asks for a password, and ask whether the same
# things are refused.
#
#   PG_BINDIR=/path/to/patched/pg19/bin pg19/test/security/run.sh
#
set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-sec-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbp-XXXXXX)"
PORT="${PGPORT:-$((7300 + RANDOM % 200))}"
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

q()  { "$PSQL" -X -q -t -A -d postgres -U postgres -c "$1" 2>&1; }

is() {
	local got; got=$(q "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

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

# login <role> <password>; prints "ok" or the whole error, on one line
login() {
	PGPASSWORD="$2" "$PSQL" -X -q -t -A -d postgres -U "$1" -c "SELECT 'ok';" 2>&1 \
		| tr '\n' ' ' | sed 's/ *$//'
}

echo "password profiles: shared labels, a revoked table and one worker"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 -U postgres \
	> "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "shared_preload_libraries = 'gp_core,gp_security'"
	echo "gp.enable_password_profile = on"
	echo "password_encryption = 'scram-sha-256'"
} >> "$WORK/data/postgresql.conf"
# A socket that asks for a password, so that a failed login is a real one.
{
	echo "local all postgres trust"
	echo "local all all scram-sha-256"
} > "$WORK/data/pg_hba.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_security CASCADE;")
case "$out" in
	*ERROR*) echo "the extension could not be created:"
	         printf '%s\n' "$out" | sed 's/^/  /'; exit 1 ;;
esac

###############################################################################
echo "1. a profile is a role with a label, and there is a default one"
###############################################################################
is "the default profile arrives with the extension" \
   "SELECT profile FROM gp_security.profiles WHERE profile = 'gp_default';" "gp_default"
is "it is a role that cannot log in" \
   "SELECT rolcanlogin FROM pg_roles WHERE rolname = 'gp_default';" "f"

# A role is the cluster's, so the extension created in a second database
# finds the first one's gp_default.  The singlenode suite found that it then
# failed, "role gp_default already exists", on its second pass.
got=$("$PSQL" -X -q -t -A -d postgres -U postgres -c "CREATE DATABASE second_db" 2>&1 &&
      "$PSQL" -X -q -t -A -d second_db -U postgres \
          -c "CREATE EXTENSION gp_core" -c "CREATE EXTENSION gp_security" \
          -c "SELECT profile FROM gp_security.profiles WHERE profile = 'gp_default'" 2>&1)
[ "$got" = "gp_default" ] && ok "and a second database's extension keeps the one it finds" \
	|| notok "and a second database's extension keeps the one it finds" "got [$got]"
isl "one can be defined with the limits Cloudberry names" \
   "SELECT gp_security.create_profile('strict',
             failed_login_attempts => 3, password_lock_time => 1,
             password_reuse_max => 2);
    SELECT failed_login_attempts || '/' || password_lock_time || '/' || password_reuse_max
      FROM gp_security.profiles WHERE profile = 'strict';" "3/1/2"
isl "and changed, leaving what was not given alone" \
   "SELECT gp_security.alter_profile('strict', failed_login_attempts => 4);
    SELECT failed_login_attempts || '/' || password_reuse_max
      FROM gp_security.profiles WHERE profile = 'strict';" "4/2"
refused "a limit outside the range Cloudberry accepts is refused" \
        "SELECT gp_security.alter_profile('strict', password_life_time => 99999);" \
        "must be between"
refused "and a setting nobody knows" \
        "SECURITY LABEL FOR gp_profile ON ROLE strict IS '{\"nonsense\": 1}';" \
        "unrecognized profile setting"
refused "a profile cannot take a name a role already has" \
        "SELECT gp_security.create_profile('postgres');" "already exists"

###############################################################################
echo "2. a role is put under one"
###############################################################################
q "CREATE ROLE alice LOGIN PASSWORD 'first-pass';" > /dev/null
isl "assigning a profile" \
   "SELECT gp_security.assign_profile('alice', 'strict');
    SELECT gp_security.role_profile('alice');" "strict"
is "which is listed the way Cloudberry lists it" \
   "SELECT rolname || ' -> ' || profile FROM gp_security.role_profiles;" "alice -> strict"
refused "a profile that does not exist is refused" \
        "SELECT gp_security.assign_profile('alice', 'nope');" \
        "profile \"nope\" does not exist"
refused "a profile with roles under it is not dropped by accident" \
        "SELECT gp_security.drop_profile('strict');" "role(s) are under it"
is "a role with no profile has none" \
   "CREATE ROLE bob LOGIN PASSWORD 'bob-pass';
    SELECT gp_security.role_profile('bob') IS NULL;" "t"

###############################################################################
echo "3. failed logins are counted, and too many lock the account"
###############################################################################
[ "$(login alice first-pass)" = "ok" ] && ok "the right password works" \
	|| notok "the right password works" "$(login alice first-pass)"
for i in 1 2 3; do login alice wrong-pass > /dev/null; done
is "three wrong ones are counted" "SELECT gp_security.role_failed_logins('alice');" "3"
is "and the account is not locked yet, because the profile says four" \
   "SELECT gp_security.role_locked_until('alice') IS NULL;" "t"
[ "$(login alice first-pass)" = "ok" ] && ok "so the right password still works" \
	|| notok "the right password still works" "$(login alice first-pass)"
is "and a good login clears what came before it" \
   "SELECT gp_security.role_failed_logins('alice');" "0"
for i in 1 2 3 4; do login alice wrong-pass > /dev/null; done
is "four wrong ones lock it" \
   "SELECT gp_security.role_locked_until('alice') IS NOT NULL;" "t"
is "for the time the profile says, not for ever" \
   "SELECT gp_security.role_locked_until('alice') < 'infinity'::timestamptz
       AND gp_security.role_locked_until('alice') > now();" "t"
case "$(login alice first-pass)" in
	*"is locked"*) ok "and the right password is refused while it is locked" ;;
	*) notok "the right password is refused while locked" "$(login alice first-pass)" ;;
esac
isl "an administrator can unlock it" \
   "SELECT gp_security.unlock_role('alice');
    SELECT gp_security.role_locked_until('alice') IS NULL;" "t"
[ "$(login alice first-pass)" = "ok" ] && ok "and then it works again" \
	|| notok "it works again" "$(login alice first-pass)"

###############################################################################
echo "4. an account can be locked by hand, and a lock is durable"
###############################################################################
isl "locking by hand locks it until someone unlocks it" \
   "SELECT gp_security.lock_role('alice');
    SELECT gp_security.role_locked_until('alice') = 'infinity'::timestamptz;" "t"
case "$(login alice first-pass)" in
	*"is locked"*) ok "and the login is refused" ;;
	*) notok "the login is refused" "$(login alice first-pass)" ;;
esac
# The worker writes the state down; give it a moment, then restart.
sleep 3
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w -t 60 restart > /dev/null 2>&1
is "the lock is in the label, so it survives a restart" \
   "SELECT gp_security.role_locked_until('alice') = 'infinity'::timestamptz;" "t"
case "$(login alice first-pass)" in
	*"is locked"*) ok "and it is still refused after the restart" ;;
	*) notok "still refused after the restart" "$(login alice first-pass)" ;;
esac
q "SECURITY LABEL FOR gp ON ROLE alice IS 'profile=strict,locked_until=2020-01-01 00:00:00+00';" > /dev/null
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w -t 60 restart > /dev/null 2>&1
[ "$(login alice first-pass)" = "ok" ] && ok "a lock whose time has passed lets the role in" \
	|| notok "a lock whose time has passed lets the role in" "$(login alice first-pass)"
sleep 12
is "and the worker tidies it away" \
   "SELECT gp_security.role_locked_until('alice') IS NULL;" "t"

###############################################################################
echo "5. what a new password must satisfy"
###############################################################################
is "the history is a table nobody else may read" \
   "SELECT has_table_privilege('public', 'gp_security.password_history', 'SELECT');" "f"
is "nothing was remembered from before the role had a profile" \
   "SELECT count(*) FROM gp_security.password_history WHERE rolname = 'alice';" "0"
isl "a password set under the profile is remembered" \
   "ALTER ROLE alice PASSWORD 'second-pass';
    SELECT count(*) FROM gp_security.password_history WHERE rolname = 'alice';" "1"
refused "and may not be set again" \
        "ALTER ROLE alice PASSWORD 'second-pass';" "may not be reused"
isl "a different one is accepted" \
   "ALTER ROLE alice PASSWORD 'third-pass';
    SELECT count(*) FROM gp_security.password_history WHERE rolname = 'alice';" "2"
refused "and now the one before it is remembered too" \
        "ALTER ROLE alice PASSWORD 'second-pass';" "may not be reused"
isl "once enough changes have gone by, an old one may be used again" \
   "ALTER ROLE alice PASSWORD 'fourth-pass';
    ALTER ROLE alice PASSWORD 'fifth-pass';
    ALTER ROLE alice PASSWORD 'second-pass';
    SELECT 'reused';" "reused"
[ "$(login alice second-pass)" = "ok" ] && ok "and the password that was set is the one that works" \
	|| notok "the password that was set works" "$(login alice second-pass)"

###############################################################################
echo "6. an already-hashed password, and a verify function"
###############################################################################
q "SELECT gp_security.alter_profile('strict', password_allow_hashed => false);" > /dev/null
refused "a hashed password is refused when the profile says so" \
        "ALTER ROLE alice PASSWORD 'md5d1e5cfcf95e5c0e4d6d7a1a1ed1d5f6b';" \
        "must be given in plain text"
isl "and allowed when it says so" \
   "SELECT gp_security.alter_profile('strict', password_allow_hashed => true);
    ALTER ROLE bob PASSWORD 'md5d1e5cfcf95e5c0e4d6d7a1a1ed1d5f6b';
    SELECT 'accepted';" "accepted"
q "CREATE FUNCTION public.no_short(username text, password text) RETURNS void
   LANGUAGE plpgsql AS \$\$
   BEGIN
     IF length(password) < 12 THEN
       RAISE EXCEPTION 'password for % is too short', username;
     END IF;
   END \$\$;
   SELECT gp_security.alter_profile('strict',
            password_verify_function => 'public.no_short');" > /dev/null
refused "a verify function is asked, and its answer is final" \
        "ALTER ROLE alice PASSWORD 'short';" "is too short"
isl "a password it accepts goes through" \
   "ALTER ROLE alice PASSWORD 'long-enough-password';
    SELECT 'accepted';" "accepted"

###############################################################################
echo "7. how long a password lasts"
###############################################################################
q "SELECT gp_security.alter_profile('strict', password_life_time => 30,
                                    password_grace_time => 3,
                                    unset => ARRAY['password_verify_function']);" > /dev/null
q "ALTER ROLE alice PASSWORD 'another-long-password';" > /dev/null
is "a setting can be taken away again by name" \
   "SELECT password_verify_function IS NULL FROM gp_security.profiles
     WHERE profile = 'strict';" "t"
is "the profile's life time becomes VALID UNTIL, with the grace period inside it" \
   "SELECT rolvaliduntil::date = (now() + interval '33 days')::date FROM pg_roles
     WHERE rolname = 'alice';" "t"
isl "a statement that says VALID UNTIL itself is left alone" \
   "ALTER ROLE alice PASSWORD 'yet-another-password' VALID UNTIL '2030-01-01';
    SELECT rolvaliduntil::date::text FROM pg_roles WHERE rolname = 'alice';" "2030-01-01"

###############################################################################
echo "8. the history is in one database, and says so"
###############################################################################
q "CREATE DATABASE other_db;" > /dev/null
out=$("$PSQL" -X -q -t -A -d other_db -U postgres \
	  -c "ALTER ROLE alice PASSWORD 'from-elsewhere';" 2>&1)
case "$out" in
	*"gp.security_database"*) ok "a password change elsewhere is refused, naming the setting" ;;
	*) notok "a password change elsewhere is refused" "$out" ;;
esac
out=$("$PSQL" -X -q -t -A -d other_db -U postgres \
	  -c "ALTER ROLE bob PASSWORD 'bob-elsewhere'; SELECT 'ok';" 2>&1 | tail -1)
[ "$out" = "ok" ] && ok "but a role whose profile has no reuse rules may change it anywhere" \
	|| notok "a role with no reuse rules may change it anywhere" "$out"

###############################################################################
echo "9. with the feature off, nothing is enforced"
###############################################################################
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w -t 60 stop > /dev/null 2>&1
sed -i "s/^gp.enable_password_profile = on/gp.enable_password_profile = off/" \
	"$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1
isl "a password that was refused is now accepted" \
   "ALTER ROLE alice PASSWORD 'first-pass';
    SELECT 'accepted';" "accepted"
is "and the profiles are still there, waiting to be turned back on" \
   "SELECT count(*) FROM gp_security.profiles;" "2"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
