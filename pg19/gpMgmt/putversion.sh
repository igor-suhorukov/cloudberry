#!/bin/sh
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
# putversion.sh <GPHOME> <version>: Cloudberry's putversion, over what meson
# installed of gpMgmt -- each "$Revision...$" a tool's --version prints
# becomes the version.  Run by "meson install", DESTDIR and all.
set -e
root="${DESTDIR:-}$1"
for dir in bin sbin lib/python/gppylib; do
	[ -d "$root/$dir" ] || continue
	grep -rl --include='*' '\$Revision' "$root/$dir" 2> /dev/null |
		while read -r f; do
			sed -i "s/\\\$Revision[^\$]*\\\$/$2/" "$f"
		done
done
