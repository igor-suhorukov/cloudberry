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
# Packs the binary release out of the two trees docker/Dockerfile.release
# installs, each under /usr/local/pgsql: the patched PostgreSQL 19 in
# <stage>/postgresql and the port's modules in <stage>/extension.  Each
# becomes a Debian package and a tarball of the same files, and the debug
# information of both, split off their programs and libraries, a third.
#
#     pg19/release/package.sh <stage> <dist>
#
# VERSION is the release's: 0.1.0, or 0.1.0-rc.1 for a pre-release.  The
# files are named with it as it is.  The packages' Version field spells a "-"
# as "~", which Debian sorts before the release, and ends in the
# distribution, so that the packages of two distributions never pass for
# each other.  Without VERSION it is a development version: the port's own
# (pg19/meson.build), the date and the commit.  MAINTAINER fills the
# packages' field of that name.
#
set -euo pipefail

stage="$1"
dist="$2"
prefix=/usr/local/pgsql
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

server=cloudberry-pg19-postgresql
extension=cloudberry-pg19-extension
dbgsym=cloudberry-pg19-dbgsym

# debian13, ubuntu24.04.  In a subshell: os-release sets VERSION too.
# shellcheck source=/dev/null
distro="$(. /etc/os-release && echo "$ID$VERSION_ID")"
arch="$(dpkg --print-architecture)"
pg_commit="$(cat "$stage/postgresql$prefix/.pg_ref_commit")"
cb_commit="$(cat "$stage/extension$prefix/.cb_ref_commit")"
pg_version="$("$stage/postgresql$prefix/bin/pg_config" --version)"

if [ -z "${VERSION:-}" ]; then
	port="$(sed -n "s/^  version: '\(.*\)',$/\1/p" "$here/../meson.build")"
	VERSION="${port%-devel}-dev.$(date -u +%Y%m%d).${cb_commit:0:7}"
fi
if ! [[ "$VERSION" =~ ^[0-9][0-9A-Za-z.]*(-[0-9A-Za-z.]+)*$ ]]; then
	echo "package.sh: \"$VERSION\" is not a version: a digit, then letters, digits and dots, in parts joined by \"-\"" >&2
	exit 1
fi
debver="${VERSION//-/\~}-1+$distro"
MAINTAINER="${MAINTAINER:-igor-suhorukov <igor-suhorukov@users.noreply.github.com>}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$dist"

# EXEC or DYN for a program or a shared library, REL for an object or a static
# library, nothing for a file that is not ELF.
elf_type() { readelf -h "$1" 2> /dev/null | awk '$1 == "Type:" { print $2; exit }'; }

# The two trees are installed into one prefix, so a file in both would be
# one that dpkg refuses to install the second package over.
both="$(comm -12 <(cd "$stage/postgresql" && find . ! -type d | sort) \
                 <(cd "$stage/extension" && find . ! -type d | sort))"
if [ -n "$both" ]; then
	echo "package.sh: the server and the modules both install" >&2
	echo "$both" >&2
	exit 1
fi

###############################################################################
# Debug information, split off as Debian's dh_strip splits it: each program
# and shared library keeps its symbol table and loses its debug sections to
# /usr/lib/debug/.build-id/, where gdb looks them up by the build ID the
# linker wrote.  A static library loses them altogether.
###############################################################################
for tree in "$stage/postgresql" "$stage/extension"; do
	while IFS= read -r -d '' f; do
		case "$(elf_type "$f")" in
		EXEC|DYN) ;;
		REL) objcopy --strip-debug --enable-deterministic-archives "$f"; continue ;;
		*) continue ;;
		esac
		id="$(readelf -n "$f" | sed -n 's/^ *Build ID: *//p')"
		if [ -z "$id" ]; then
			echo "package.sh: ${f#"$tree"} has no build ID" >&2
			exit 1
		fi
		debug="$work/dbgsym/usr/lib/debug/.build-id/${id:0:2}/${id:2}.debug"
		mkdir -p "${debug%/*}"
		objcopy --only-keep-debug --compress-debug-sections "$f" "$debug"
		chmod 644 "$debug"
		objcopy --strip-debug --remove-section=.comment --remove-section=.note \
			--add-gnu-debuglink="$debug" "$f"
	done < <(find "$tree" -type f -print0)
done

###############################################################################
# The tarballs.  Each holds the prefix and nothing above it, so that
# unpacking one into / sets no directory's owner or mode but its own.
###############################################################################
tarball() {	# tarball <file> <root> <directory in it>
	tar -C "$2" --sort=name --owner=0 --group=0 --numeric-owner -czf "$dist/$1" "$3"
}
tarball "$server-$VERSION-$distro-$arch.tar.gz" "$stage/postgresql" "${prefix#/}"
tarball "$extension-$VERSION-$distro-$arch.tar.gz" "$stage/extension" "${prefix#/}"
tarball "$dbgsym-$VERSION-$distro-$arch.tar.gz" "$work/dbgsym" usr/lib/debug

###############################################################################
# The Debian packages.  dpkg-shlibdeps names the distribution's packages the
# programs and libraries need; it reads their names from a debian/control,
# and debian/shlibs.local tells it that the libraries the server installs --
# libpq, which psql and gp_core's dispatcher load, and ecpg's -- are the
# server package's.
###############################################################################
mkdir -p "$work/src/debian"
printf 'Source: cloudberry-pg19\nMaintainer: %s\n\nPackage: %s\nArchitecture: any\n\nPackage: %s\nArchitecture: any\n' \
	"$MAINTAINER" "$server" "$extension" > "$work/src/debian/control"
for lib in "$stage/postgresql$prefix"/lib/lib*.so.*; do
	[ -L "$lib" ] && continue
	soname="$(objdump -p "$lib" | awk '$1 == "SONAME" { print $2 }')"
	echo "${soname%%.so.*} ${soname##*.so.} $server (= $debver)"
done | sort -u > "$work/src/debian/shlibs.local"

depends() {	# depends <tree>: what its ELF files need, but the server package
	local files=() f
	while IFS= read -r -d '' f; do
		case "$(elf_type "$f")" in EXEC|DYN) files+=("-e$f") ;; esac
	done < <(find "$1" -type f -print0)
	# --warnings=0, since every module uses the server's own symbols, which
	# no library has; and the grep drops the warning, given once per file,
	# that the files are not in a debian/<package> directory.
	(cd "$work/src" && dpkg-shlibdeps -O --warnings=0 -x"$server" \
		-l"$stage/postgresql$prefix/lib" "${files[@]}" \
		2> >(grep -v "should already be installed in their package's directory" >&2)) |
		sed -n 's/^shlibs:Depends=//p'
}

deb() {	# deb <package> <root>; the rest of its control file on stdin
	local size
	size="$(du -sk "$2" | cut -f1)"
	mkdir "$2/DEBIAN"
	{
		echo "Package: $1"
		echo "Version: $debver"
		echo "Architecture: $arch"
		echo "Maintainer: $MAINTAINER"
		echo "Installed-Size: $size"
		echo "Homepage: https://github.com/igor-suhorukov/cloudberry/tree/extension_postgresql_19/pg19"
		cat
	} > "$2/DEBIAN/control"
	dpkg-deb --root-owner-group -Zxz --build "$2" "$dist/${1}_$VERSION-${distro}_$arch.deb" > /dev/null
	rm -r "$2/DEBIAN"
}

# Outside the here-documents, where a failure would not stop the script.
server_depends="$(depends "$stage/postgresql")"
extension_depends="$(depends "$stage/extension")"
if [ -z "$server_depends" ] || [ -z "$extension_depends" ]; then
	echo "package.sh: dpkg-shlibdeps found no libraries the packages need" >&2
	exit 1
fi

deb "$server" "$stage/postgresql" <<EOF
Depends: $server_depends
Section: database
Priority: optional
Description: PostgreSQL 19 with Apache Cloudberry's core patch series
 $pg_version with the patch series of branch REL_19_STABLE_CLOUDBERRY,
 commit $pg_commit:
 the hooks the Cloudberry on PostgreSQL 19 port's modules are written
 against, which leave it vanilla PostgreSQL 19 while no module uses them.
 Installed in $prefix; $extension is the port.
EOF

deb "$extension" "$stage/extension" <<EOF
Depends: $server (= $debver), $extension_depends
Recommends: python3, python3-pygresql, python3-psutil, python3-yaml, python3-pexpect, openssh-client, rsync, iproute2, iputils-ping, less
Suggests: etcd-server, fakeroot
Section: database
Priority: optional
Description: Apache Cloudberry as extensions of PostgreSQL 19
 The Cloudberry on PostgreSQL 19 port, commit
 $cb_commit:
 its modules -- gp_core's cluster and dispatcher, ORCA, the AO and PAX
 table access methods, the interconnect, resource groups and the rest --
 and Cloudberry's management tools, gpMgmt, in $prefix beside the
 server of $server, the only one they load in.
 The tools want the Python modules recommended, and ssh to each host of a
 cluster.
EOF

deb "$dbgsym" "$work/dbgsym" <<EOF
Depends: $server (= $debver)
Section: debug
Priority: optional
Description: debug symbols of $server and $extension
 The debug information of their programs and libraries, which gdb finds by
 their build IDs.
EOF

ls -l "$dist"
