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

# UDP2's core, Cloudberry's contrib/udp2/ic_common, for the udp2 module.
#
# It is C++ that includes no PostgreSQL header, so it is compiled where it
# lies, unmodified, into a static library the module links, and the module's
# adapter (modules/udp2) is all the port writes.  Its symbols are hidden:
# PostgreSQL opens a module with RTLD_GLOBAL, and the core's global_param,
# session_param, GetLastError() and the rest would otherwise be every
# module's.  Its assertions, and the test mode in which it loses packets as
# gp.udpic_drop* ask, follow the server's, as Cloudberry's CMake build
# (ic_common/CMakeLists.txt) follows its --enable-cassert.

udp2_root = '..' / '..' / 'contrib' / 'udp2' / 'ic_common'

udp2_inc = include_directories(udp2_root, udp2_root / 'udp2')

udp2_core = static_library('udp2_core',
  files(udp2_root / 'ic_utility.cpp', udp2_root / 'udp2' / 'ic_udp2.cpp'),
  cpp_args: ['-D_GNU_SOURCE', '-fno-strict-aliasing', '-fwrapv'] +
            (pg_cassert ? [] : ['-DNDEBUG']),
  include_directories: udp2_inc,
  gnu_symbol_visibility: 'hidden',
  pic: true,
)
