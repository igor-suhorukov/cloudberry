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
# Password profiles, account locking, password rules and DENY windows.
#
# Cloudberry keeps all of this in three shared catalogs, seven pg_authid
# columns and a pair of postmaster children.  Here it is shared security
# labels, a table nobody may read, and one background worker -- so these tests
# log in for real, over a socket that asks for a password or an OAuth token,
# and ask whether the same things are refused.
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

# The OAuth validator and client of the port's tests, where they are built
# (meson's hook_tests); the OAuth checks are skipped where they are not.
PKGLIB="$("$BINDIR/pg_config" --pkglibdir)"
OAUTH=
[ -f "$PKGLIB/gp_oauth_probe.so" ] && [ -x "$BINDIR/gp_oauth_client" ] && OAUTH=1

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 -U postgres \
	> "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	# gp_sql for Cloudberry's spelling of DENY, which O26 rewrites
	echo "shared_preload_libraries = 'gp_core,gp_sql,gp_security'"
	echo "gp.enable_password_profile = on"
	echo "password_encryption = 'scram-sha-256'"
	# the secret a dispatcher's connection proves itself with (section 9)
	echo "gp.cluster_secret = 'security-suite-secret-0123456789'"
	[ -n "$OAUTH" ] && echo "oauth_validator_libraries = 'gp_oauth_probe'"
} >> "$WORK/data/postgresql.conf"
# A socket that asks for a password, so that a failed login is a real one --
# and for oscar a token, which gp_oauth_probe takes if it is "good".
{
	echo "local all postgres trust"
	[ -n "$OAUTH" ] && echo 'local all oscar oauth issuer="https://256.256.256.256" scope="openid"'
	echo "local all all scram-sha-256"
} > "$WORK/data/pg_hba.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

out=$(q "CREATE EXTENSION gp_security CASCADE; CREATE EXTENSION gp_sql;")
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
   "CALL gp_security.create_profile('strict',
             failed_login_attempts => 3, password_lock_time => 1,
             password_reuse_max => 2);
    SELECT failed_login_attempts || '/' || password_lock_time || '/' || password_reuse_max
      FROM gp_security.profiles WHERE profile = 'strict';" "3/1/2"
isl "and changed, leaving what was not given alone" \
   "CALL gp_security.alter_profile('strict', failed_login_attempts => 4);
    SELECT failed_login_attempts || '/' || password_reuse_max
      FROM gp_security.profiles WHERE profile = 'strict';" "4/2"
refused "a limit outside the range Cloudberry accepts is refused" \
        "CALL gp_security.alter_profile('strict', password_life_time => 99999);" \
        "must be between"
refused "and a setting nobody knows" \
        "SECURITY LABEL FOR gp_profile ON ROLE strict IS '{\"nonsense\": 1}';" \
        "unrecognized profile setting"
refused "a profile cannot take a name a role already has" \
        "CALL gp_security.create_profile('postgres');" "already exists"

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
        "CALL gp_security.drop_profile('{strict}');" "role(s) are under it"
is "a role with no profile has none" \
   "CREATE ROLE bob LOGIN PASSWORD 'bob-pass';
    SELECT gp_security.role_profile('bob') IS NULL;" "t"

# Cloudberry's switch: a role's profile holds it only once ENABLE PROFILE has
# turned it on (pg_authid.rolenableprofile, false for a new role) -- its own
# profile, or the default one for a role with none.
for i in 1 2; do login alice wrong-pass > /dev/null; done
is "a profile does not hold its role before ENABLE PROFILE: no failed login is counted" \
   "SELECT gp_security.role_failed_logins('alice');" "0"
isl "ALTER USER ... ENABLE PROFILE turns it on" \
   "ALTER USER alice ENABLE PROFILE;
    SELECT rolname || ' ' || profile || ' ' || rolenableprofile
      FROM gp_security.role_profiles WHERE rolname = 'alice';" "alice strict true"
isl "CREATE USER ... ENABLE PROFILE PROFILE p, and ALTER USER ... DISABLE PROFILE PROFILE p, as Cloudberry writes them" \
   "CREATE USER carol ENABLE PROFILE PROFILE strict;
    ALTER USER carol DISABLE PROFILE PROFILE gp_default;
    SELECT rolname || ' ' || profile || ' ' || rolenableprofile
      FROM gp_security.role_profiles WHERE rolname = 'carol';" "carol gp_default false"
refused "a profile named alone after ENABLE PROFILE is Cloudberry's syntax error" \
        "ALTER USER carol ENABLE PROFILE PROFILE;" "syntax error"
default_fla=$(q "SELECT coalesce(failed_login_attempts::text, 'unset') FROM gp_security.profiles WHERE profile = 'gp_default';")
q "CREATE ROLE eve LOGIN PASSWORD 'eve-pass' ENABLE PROFILE;
   CALL gp_security.alter_profile('gp_default', failed_login_attempts => 2);" > /dev/null
for i in 1 2; do login eve wrong-pass > /dev/null; done
case "$(login eve eve-pass)" in
	*"is locked"*) ok "a role with no profile of its own, switched on, is held to the default profile" ;;
	*) notok "ENABLE PROFILE with no profile holds the role to the default profile" "$(login eve eve-pass)" ;;
esac
if [ "$default_fla" = unset ]; then
	q "CALL gp_security.alter_profile('gp_default', unset => ARRAY['failed_login_attempts']);" > /dev/null
else
	q "CALL gp_security.alter_profile('gp_default', failed_login_attempts => $default_fla);" > /dev/null
fi
isl "and DISABLE PROFILE lets it go" \
   "SELECT gp_security.unlock_role('eve');
    ALTER USER eve DISABLE PROFILE;
    SELECT gp_security.role_profile_enabled('eve');" "f"
for i in 1 2 3; do login eve wrong-pass > /dev/null; done
[ "$(login eve eve-pass)" = "ok" ] && ok "after which its failed logins lock nothing" \
	|| notok "a role whose profile is off is not locked" "$(login eve eve-pass)"

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
q "SECURITY LABEL FOR gp ON ROLE alice IS 'profile=strict,locked_until=2020-01-01 00:00:00+00,enable_profile';" > /dev/null
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
q "CALL gp_security.alter_profile('strict', password_allow_hashed => false);" > /dev/null
refused "a hashed password is refused when the profile says so" \
        "ALTER ROLE alice PASSWORD 'md5d1e5cfcf95e5c0e4d6d7a1a1ed1d5f6b';" \
        "must be given in plain text"
isl "and allowed when it says so" \
   "CALL gp_security.alter_profile('strict', password_allow_hashed => true);
    ALTER ROLE bob PASSWORD 'md5d1e5cfcf95e5c0e4d6d7a1a1ed1d5f6b';
    SELECT 'accepted';" "accepted"
q "CREATE FUNCTION public.no_short(username text, password text) RETURNS void
   LANGUAGE plpgsql AS \$\$
   BEGIN
     IF length(password) < 12 THEN
       RAISE EXCEPTION 'password for % is too short', username;
     END IF;
   END \$\$;
   CALL gp_security.alter_profile('strict',
            password_verify_function => 'public.no_short');" > /dev/null
refused "a verify function is asked, and its answer is final" \
        "ALTER ROLE alice PASSWORD 'short';" "is too short"
isl "a password it accepts goes through" \
   "ALTER ROLE alice PASSWORD 'long-enough-password';
    SELECT 'accepted';" "accepted"

###############################################################################
echo "7. how long a password lasts"
###############################################################################
q "CALL gp_security.alter_profile('strict', password_life_time => 30,
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
echo "9. a login at a time its role's DENY windows forbid"
###############################################################################
q "CREATE ROLE dora LOGIN PASSWORD 'dora-pass' DENY BETWEEN DAY 0 AND DAY 6;
   SELECT gp_security.assign_profile('dora', 'strict');
   SELECT gp_security.enable_profile('dora', true);" > /dev/null
is "a DENY window is the role's, in Cloudberry's catalog by its name" \
   "SELECT start_day || ' ' || start_time || ' ' || end_day || ' ' || end_time
      FROM pg_auth_time_constraint WHERE authid = 'dora'::regrole;" "0 00:00:00 6 24:00:00"
case "$(login dora dora-pass)" in
	*"authentication failed for user \"dora\": login not permitted at this time"*)
		ok "a login inside it is refused, the password right" ;;
	*) notok "a login inside a DENY window is refused" "$(login dora dora-pass)" ;;
esac
is "and it is no failed login" "SELECT gp_security.role_failed_logins('dora');" "0"
case "$(PGOPTIONS="-c gp.qe_identity=seg0/dbid2/sess1" login dora dora-pass)" in
	*"login not permitted at this time"*)
		ok "a connection that only says it is the dispatcher's is refused too" ;;
	*) notok "gp.qe_identity alone should not pass a DENY window" \
	         "$(PGOPTIONS="-c gp.qe_identity=seg0/dbid2/sess1" login dora dora-pass)" ;;
esac
case "$(PGOPTIONS="-c gp.qe_identity=seg0/dbid2/sess1 -c gp.qe_secret=security-suite-secret-0123456789" \
		login dora dora-pass)" in
	*"login not permitted"*) notok "the dispatcher's connection, with the secret, is not asked" \
	                           "it was refused" ;;
	*) ok "the dispatcher's own connection, with the cluster's secret, is not asked" ;;
esac
isl "DROP DENY takes away the windows it meets" \
   "ALTER ROLE dora DROP DENY FOR DAY 3;
    SELECT count(*) FROM pg_auth_time_constraint WHERE authid = 'dora'::regrole;" "0"
[ "$(login dora dora-pass)" = "ok" ] && ok "and the login goes through" \
	|| notok "the login goes through with no window" "$(login dora dora-pass)"
# The day, in the server's time zone, from gp.auth_time_override, as
# Cloudberry's gp_auth_time_override gives it: 2011-08-30 was a Tuesday.
q "ALTER ROLE dora DENY DAY 'Tuesday';" > /dev/null
echo "gp.auth_time_override = '2011-08-30 12:00:00'" >> "$WORK/data/postgresql.conf"
q "SELECT pg_reload_conf();" > /dev/null; sleep 1
case "$(login dora dora-pass)" in
	*"login not permitted at this time"*) ok "a window of a day refuses a login that day" ;;
	*) notok "DENY DAY 'Tuesday' refuses a login on a Tuesday" "$(login dora dora-pass)" ;;
esac
sed -i "s/^gp.auth_time_override = .*/gp.auth_time_override = '2011-08-31 12:00:00'/" \
	"$WORK/data/postgresql.conf"
q "SELECT pg_reload_conf();" > /dev/null; sleep 1
[ "$(login dora dora-pass)" = "ok" ] && ok "and lets one in the day after" \
	|| notok "DENY DAY 'Tuesday' lets a login in on a Wednesday" "$(login dora dora-pass)"
sed -i "/^gp.auth_time_override = /d" "$WORK/data/postgresql.conf"
q "SELECT pg_reload_conf();" > /dev/null
q "CREATE ROLE sam LOGIN SUPERUSER PASSWORD 'sam-pass';
   SET allow_system_table_mods = on;
   INSERT INTO pg_auth_time_constraint
        SELECT 'sam'::regrole, 0, '00:00:00', 6, '24:00:00';" > /dev/null
case "$(login sam sam-pass)" in
	*"login not permitted at this time"*) ok "a superuser a catalog write gave a window is refused too" ;;
	*) notok "a superuser with a window is refused" "$(login sam sam-pass)" ;;
esac
# The login's warning goes to the log alone, which a restart without -l
# (section 4) has left behind; the check SQL asks gives it to the client.
refused "with Cloudberry's warning" \
        "SELECT gp_security.check_auth_time_constraints('sam', now());" \
        "WARNING:  time constraints added on superuser role"
isl "a catalog write takes it away again" \
   "SET allow_system_table_mods = on;
    DELETE FROM pg_auth_time_constraint WHERE authid = 'sam'::regrole;
    SELECT count(*) FROM pg_auth_time_constraint WHERE authid = 'sam'::regrole;" "0"
[ "$(login sam sam-pass)" = "ok" ] && ok "and the superuser logs in" \
	|| notok "the superuser logs in again" "$(login sam sam-pass)"
case "$("$BINDIR/pg_dumpall" -U postgres --roles-only 2>&1 | grep "ON ROLE dora")" in
	*"deny=2 00:00:00 2 24:00:00"*) ok "pg_dumpall writes a role's windows with it" ;;
	*) notok "pg_dumpall should write dora's window" \
	         "$("$BINDIR/pg_dumpall" -U postgres --roles-only 2>&1 | grep dora)" ;;
esac
# Kept, the Tuesday window would hold for the rest of the run on a Tuesday,
# the time override gone: section 11 logs dora in again.
q "ALTER ROLE dora DROP DENY FOR DAY 2;" > /dev/null

###############################################################################
echo "10. what is not a failed login"
###############################################################################
# psql -w: a password asked for and none given, which libpq answers by
# hanging up -- as it does before it asks its user for one.
q "SELECT gp_security.unlock_role('alice');" > /dev/null
for i in 1 2 3 4 5; do
	PGPASSWORD= "$PSQL" -X -w -q -d postgres -U alice -c "SELECT 1" > /dev/null 2>&1
done
is "a client that hangs up is not counted" "SELECT gp_security.role_failed_logins('alice');" "0"
login alice wrong-pass > /dev/null
is "a wrong password still is" "SELECT gp_security.role_failed_logins('alice');" "1"
q "SELECT gp_security.unlock_role('alice');" > /dev/null
if [ -z "$OAUTH" ]; then
	echo "  skip   OAuth logins: gp_oauth_probe and gp_oauth_client are not built (meson's hook_tests)"
else
	q "CALL gp_security.create_profile('three', failed_login_attempts => 3);
	   CREATE ROLE oscar LOGIN ENABLE PROFILE;
	   SELECT gp_security.assign_profile('oscar', 'three');" > /dev/null
	issuer="host=$SOCK port=$PORT dbname=postgres user=oscar oauth_issuer=https://256.256.256.256 oauth_client_id=suite"
	# psql has no token: libpq's discovery round trip, then it gives up
	for i in 1 2 3 4; do
		PGOAUTHDEBUG=UNSAFE "$PSQL" -X -d "$issuer" -c "SELECT 1" > /dev/null 2>&1
	done
	is "OAuth's discovery round trip is not counted" \
	   "SELECT gp_security.role_failed_logins('oscar');" "0"
	out=$(PGOAUTHDEBUG=UNSAFE "$BINDIR/gp_oauth_client" good "$issuer" 2>&1)
	[ "$out" = "ok" ] && ok "a good token logs in, after its discovery" \
		|| notok "a good token logs in" "$out"
	PGOAUTHDEBUG=UNSAFE "$BINDIR/gp_oauth_client" bad "$issuer" > /dev/null 2>&1
	sleep 0.5
	is "a refused token is counted, once, its discovery not" \
	   "SELECT gp_security.role_failed_logins('oscar');" "1"
	for i in 1 2; do
		PGOAUTHDEBUG=UNSAFE "$BINDIR/gp_oauth_client" bad "$issuer" > /dev/null 2>&1
	done
	sleep 0.5
	case "$(PGOAUTHDEBUG=UNSAFE "$BINDIR/gp_oauth_client" good "$issuer" 2>&1)" in
		*"is locked"*) ok "and as many as the profile allows lock the role" ;;
		*) notok "three refused tokens lock oscar" \
		         "$(PGOAUTHDEBUG=UNSAFE "$BINDIR/gp_oauth_client" good "$issuer" 2>&1)" ;;
	esac
fi

###############################################################################
echo "11. with the feature off, nothing is enforced"
###############################################################################
"$BINDIR/pg_ctl" -D "$WORK/data" -m fast -w -t 60 stop > /dev/null 2>&1
sed -i "s/^gp.enable_password_profile = on/gp.enable_password_profile = off/" \
	"$WORK/data/postgresql.conf"
"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1
isl "a password that was refused is now accepted" \
   "ALTER ROLE alice PASSWORD 'first-pass';
    SELECT 'accepted';" "accepted"
is "and the profiles are still there, waiting to be turned back on" \
   "SELECT count(*) FROM gp_security.profiles;" "$([ -n "$OAUTH" ] && echo 3 || echo 2)"
case "$(login dora dora-pass)" in
	*"login not permitted at this time"*) notok "a DENY window is not a profile's" "it refused" ;;
	*) ;;
esac
q "ALTER ROLE dora DENY BETWEEN DAY 0 AND DAY 6;" > /dev/null
case "$(login dora dora-pass)" in
	*"login not permitted at this time"*) ok "but a DENY window, which is no profile's, still is" ;;
	*) notok "a DENY window is enforced with profiles off" "$(login dora dora-pass)" ;;
esac

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
