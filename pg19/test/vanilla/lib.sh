#-------------------------------------------------------------------------
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
#-------------------------------------------------------------------------

#
# Helpers shared by the vanilla-equivalence checks.
#
# Each check is a script in checks/ that sources this file.  It reports with
# ok/notok/skip, which print a line and record it, so that run.sh can tally
# across checks that ran in separate processes.
#
# The contract a check is given:
#
#   PG_VANILLA   prefix of the unpatched PostgreSQL
#   PG_PATCHED   prefix of the PostgreSQL carrying the patch series
#   WORKDIR      scratch directory, already created
#   RESULTS      file the counters are appended to
#

: "${PG_VANILLA:?PG_VANILLA is not set}"
: "${PG_PATCHED:?PG_PATCHED is not set}"
: "${WORKDIR:?WORKDIR is not set}"
: "${RESULTS:=$WORKDIR/results}"

# A Unix socket path may be at most 107 bytes, and WORKDIR can be anywhere, so
# the sockets get a short directory of their own rather than living beside the
# data directories.
if [ -z "${SOCKDIR:-}" ]; then
	SOCKDIR="$(mktemp -d /tmp/cbs-XXXXXX)"
	export SOCKDIR
fi

ok()   { printf '  ok     %s\n' "$1"; echo "ok" >> "$RESULTS"; }
skip() { printf '  skip   %s\n' "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; echo "skip" >> "$RESULTS"; }

notok() {
	printf '  NOT OK %s\n' "$1"
	if [ -n "${2:-}" ]; then
		printf '%s\n' "$2" | head -20 | sed 's/^/         /'
	fi
	echo "notok" >> "$RESULTS"
}

# compare <label> <file-a> <file-b>
#		Two outputs that have to be identical, with the first lines that
#		differ shown when they are not.
compare() {
	if cmp -s "$2" "$3"; then
		ok "$1"
	else
		notok "$1" "$(diff -u "$2" "$3" | head -20)"
	fi
}

###############################################################################
# Running a server from either installation
###############################################################################

# Each installation is under its own prefix, so the tools have to be told
# where their libpq is.
pg_env() {								# pg_env <prefix>
	printf 'LD_LIBRARY_PATH=%s' "$("$1/bin/pg_config" --libdir)"
}

pg_run() {								# pg_run <prefix> <program> [args...]
	local prefix="$1"; shift
	local prog="$1"; shift
	env "$(pg_env "$prefix")" "$prefix/bin/$prog" "$@"
}

pg_init() {								# pg_init <prefix> <datadir>
	pg_run "$1" initdb -D "$2" -N --locale=C --encoding=UTF8 \
		> "$2.initdb.log" 2>&1
}

# pg_start <prefix> <datadir> <port> [conf line]...
pg_start() {
	local prefix="$1" datadir="$2" port="$3"; shift 3
	{
		echo "unix_socket_directories = '$SOCKDIR'"
		echo "listen_addresses = ''"
		echo "port = $port"
		for line in "$@"; do echo "$line"; done
	} >> "$datadir/postgresql.conf"
	pg_run "$prefix" pg_ctl -D "$datadir" -l "$datadir.log" -w -t 60 start \
		> /dev/null 2>&1
}

pg_stop() {								# pg_stop <prefix> <datadir>
	pg_run "$1" pg_ctl -D "$2" -m fast -w -t 60 stop > /dev/null 2>&1
}

# psql_at <prefix> <port> <sql>	 -- one value or one result set, unadorned
psql_at() {
	PGHOST="$SOCKDIR" pg_run "$1" psql -X -q -t -A -p "$2" -d postgres -c "$3" 2>&1
}

# psql_file <prefix> <port> <file>
psql_file() {
	PGHOST="$SOCKDIR" pg_run "$1" psql -X -q -t -A -p "$2" -d postgres -f "$3" 2>&1
}

# Ports.  Two servers of the same check may run at once, so they are derived
# from the check's own name rather than picked at random.
port_for() {							# port_for <name> <offset>
	local base=$(( 5700 + ($(cksum <<< "$1" | cut -d' ' -f1) % 200) ))
	echo $(( base * 2 % 1000 + 6000 + ${2:-0} ))
}
