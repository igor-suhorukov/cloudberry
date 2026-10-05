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
# The binary release's smoke test, run as root on a clean system of the
# distribution the packages were built for:
#
#     docker run --rm -v "$PWD/dist:/dist:ro" -v "$PWD/pg19:/pg19:ro" \
#         debian:trixie-slim bash /pg19/release/smoke.sh
#
# apt installs the server's and the modules' packages from /dist, and the
# distribution's packages they depend on and nothing they only recommend;
# every program and library installed then finds each library it links; the
# tarballs hold the files the packages do; and the port's M0 load tests
# (test/load/run.sh) start servers with every module and create every
# extension.
#
set -euo pipefail

prefix=/usr/local/pgsql
export DEBIAN_FRONTEND=noninteractive

one() {		# one <glob>: the one file in /dist it matches
	# shellcheck disable=SC2206  # $1 is a pattern, to be expanded
	local files=(/dist/$1)
	if [ "${#files[@]}" -ne 1 ] || [ ! -f "${files[0]}" ]; then
		echo "smoke.sh: /dist should hold one $1, and holds: ${files[*]}" >&2
		exit 1
	fi
	echo "${files[0]}"
}

echo "== installing the packages"
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
	"$(one 'cloudberry-pg19-postgresql_*.deb')" "$(one 'cloudberry-pg19-extension_*.deb')" \
	"$(one 'cloudberry-pg19-dbgsym_*.deb')" > /dev/null
dpkg-query -W cloudberry-pg19-postgresql cloudberry-pg19-extension cloudberry-pg19-dbgsym
[ -n "$(find /usr/lib/debug/.build-id -name '*.debug' -print -quit)" ]

echo "== every program and library finds its libraries"
missing="$(find "$prefix" -type f \( -name '*.so*' -o -perm -u+x \) -print0 |
	xargs -0 ldd 2> /dev/null | awk '/^\// { file = $1 } /not found/ { print file, $1 }')" || true
if [ -n "$missing" ]; then
	echo "smoke.sh: libraries not found:" >&2
	echo "$missing" >&2
	exit 1
fi

echo "== the tarballs hold what the packages hold"
for p in postgresql extension dbgsym; do
	# A package's paths start with "./", and it has "./" itself.
	diff <(dpkg-deb --fsys-tarfile "$(one "cloudberry-pg19-${p}_*.deb")" | tar -t |
	           sed -e 's|^\./||' -e '/^$/d' | grep -v '/$' | sort) \
	     <(tar -tzf "$(one "cloudberry-pg19-$p-*.tar.gz")" | grep -v '/$' | sort)
done

echo "== the M0 load tests"
id smoke > /dev/null 2>&1 || useradd -m smoke
runuser -u smoke -- env PG_BINDIR="$prefix/bin" TMPDIR=/tmp bash /pg19/test/load/run.sh
