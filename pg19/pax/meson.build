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

# PAX, Cloudberry's contrib/pax_storage, for the pax module.
#
# As ORCA's, two halves:
#
#   Cloudberry's files  compiled where they are, unmodified, wherever they
#                       compile against PostgreSQL 19 once the headers in
#                       include/ stand in for Cloudberry's: comm/cbdb_api.h,
#                       PAX's one header of PostgreSQL's, and pax_compat.h,
#                       Cloudberry's spellings over PostgreSQL 19's.
#
#   the port's copies   of the files that had to change, under src/ at the
#                       same paths, each saying at its head what changed.  A
#                       path in sources.txt compiles from src/ where the
#                       port has a copy, and from Cloudberry's tree otherwise.
#
# Cloudberry's originals stay where they are, so a merge from
# apache/cloudberry still applies to them; a change upstream makes to a file
# the port has copied is ported by hand, as for ORCA's translator.

fs = import('fs')

pax_root = '..' / '..' / 'contrib' / 'pax_storage' / 'src' / 'cpp'

protobuf_dep = dependency('protobuf', required: true)
protoc = find_program('protoc', required: true)
# io_uring, which PAX's reader uses to read a file's groups at once; it
# falls back to pread where the kernel refuses a ring (comm/fast_io.h).
uring_dep = dependency('liburing', required: true)
pax_lz4_dep = dependency('liblz4', required: false)

subdir('gen' / 'storage' / 'proto')

# the generated headers too, so that ninja makes them before what includes them
pax_sources = [pax_proto_sources]
foreach line : fs.read('sources.txt').strip().split('\n')
  path = line.strip()
  if path == '' or path.startswith('#')
    continue
  endif
  if fs.is_file('src' / path)
    pax_sources += files('src' / path)
  else
    pax_sources += files(pax_root / path)
  endif
endforeach

# The port's headers first: its copies shadow Cloudberry's of the same path.
if fs.is_dir('src')
  pax_inc = include_directories('include', 'src', pax_root, 'gen')
else
  pax_inc = include_directories('include', pax_root, 'gen')
endif

pax_cpp_args = [
  '-DUSE_PAX_CATALOG',
  # PAX's own warning set, as its CMakeLists.txt builds it, less -Werror:
  # upstream C++ the port does not edit, as ORCA's is.
  '-Wno-unused-function',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-sign-compare',
  '-Wno-clobbered',
  '-Wno-sized-deallocation',
  # PostgreSQL opens every library with RTLD_GLOBAL, so what a library
  # exports it shares with the whole backend: PAX's C++ symbols stay its
  # own, as ORCA's do (rule 8 of "Core patches that keep vanilla
  # behaviour", applied between libraries).  PG_FUNCTION_INFO_V1 and
  # _PG_init are PGDLLEXPORT, so they stay reachable.
  '-fvisibility=hidden',
  '-fvisibility-inlines-hidden',
]
# PAX's debug checks follow the server's assertions, as Cloudberry's
# CMakeLists.txt turns them on where pg_config.h has USE_ASSERT_CHECKING.
if pg_cassert
  pax_cpp_args += '-DENABLE_DEBUG'
endif
if pax_lz4_dep.found()
  pax_cpp_args += '-DUSE_LZ4'
endif

pax_deps = [protobuf_dep, uring_dep, zstd_dep, zlib_dep]
if pax_lz4_dep.found()
  pax_deps += pax_lz4_dep
endif

# PAX's dump functions -- dump_pax_file_desc() and the rest of
# storage/micro_partition_udf.cc, over its ORC dump reader -- which print
# through tabulate, a header-only library Cloudberry carries as a git
# submodule of its tree (contrib/tabulate).  A git archive of the tree has
# none, so the image clones it at the commit the tree's submodule names
# (Dockerfile.cbext); where it is not there, PAX is built without them.
tabulate_dir = pax_root / 'contrib' / 'tabulate' / 'include'
if fs.is_file(tabulate_dir / 'tabulate' / 'table.hpp')
  foreach path : ['storage/orc/orc_dump_reader.cpp', 'storage/micro_partition_udf.cc']
    if fs.is_file('src' / path)
      pax_sources += files('src' / path)
    else
      pax_sources += files(pax_root / path)
    endif
  endforeach
  pax_inc = [pax_inc, include_directories(tabulate_dir)]
  pax_cpp_args += '-DPAX_DUMP_FUNCTIONS'
endif
