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

# ORCA for gp_orca.
#
# Two halves, and the split is the point:
#
#   the core       libgpos, libnaucrates, libgpopt, libgpdbcost, compiled from
#                  Cloudberry's tree at their own paths, unmodified.  Nothing
#                  under them includes a PostgreSQL header, so there is no
#                  PostgreSQL 16 in them to port.
#
#   the translator Query/plan <-> DXL, which is all of the PostgreSQL coupling
#                  there is.  It lives under translate/ here, as the port's own
#                  code, because it has to differ from Cloudberry's: that is
#                  where PostgreSQL 16 became PostgreSQL 19.
#
# See ../README.md, "ORCA".

fs = import('fs')

###############################################################################
# xerces-c, which the DXL parser is written against
###############################################################################

xerces_dep = dependency('xerces-c', required: false)
if not xerces_dep.found()
  # Debian ships no pkg-config file for xerces-c, so fall back to the header
  # and the library by name.
  xerces_dep = cpp.find_library('xerces-c', required: true,
                                has_headers: ['xercesc/util/XercesVersion.hpp'])
endif

###############################################################################
# The core: Cloudberry's ORCA, unmodified
###############################################################################

orca_root = '..' / '..' / 'src' / 'backend' / 'gporca'

orca_core_inc = include_directories(
  orca_root / 'libgpos' / 'include',
  orca_root / 'libnaucrates' / 'include',
  orca_root / 'libgpopt' / 'include',
  orca_root / 'libgpdbcost' / 'include',
)

# The source list is a checked-in file rather than a glob; see
# list-core-sources.sh for why, and for how to refresh it.
orca_core_sources = []
foreach line : fs.read('core_sources.txt').strip().split('\n')
  path = line.strip()
  if path != '' and not path.startswith('#')
    orca_core_sources += files(path)
  endif
endforeach

if orca_core_sources.length() == 0
  error('pg19/orca/core_sources.txt lists no sources')
endif

# ORCA's own warning set, as Cloudberry and pgorca both build it.  Not the
# port's: this is upstream C++ that the port does not edit, so a warning here
# is not something the port can fix, and -Werror on it would break the build on
# every new compiler release.
orca_cpp_args = [
  '-DUSE_CMAKE',
  '-Wno-unused-parameter',
  '-Wno-sign-compare',
  # PostgreSQL opens every library with RTLD_GLOBAL, so what a library exports
  # it shares with the whole backend.  ORCA is 380k lines of C++ and would
  # otherwise put every one of its symbols there.  This is rule 8 of "Core
  # patches that keep vanilla behaviour" applied between libraries, as gp_task
  # does for the Vixie cron globals.  The module's C sources keep default
  # visibility, so PG_FUNCTION_INFO_V1 and _PG_init stay reachable.
  '-fvisibility=hidden',
  '-fvisibility-inlines-hidden',
]
if get_option('buildtype').startswith('debug')
  orca_cpp_args += '-DGPOS_DEBUG'
endif

orca_core = static_library('gporca_core',
  orca_core_sources,
  cpp_args: orca_cpp_args,
  include_directories: orca_core_inc,
  dependencies: [xerces_dep],
  # The extension is a shared module, so everything linked into it is
  # position-independent; and hidden visibility keeps ORCA's C++ symbols out of
  # the namespace PostgreSQL shares between libraries (rule 8 of "Core patches
  # that keep vanilla behaviour", applied between libraries as gp_task does).
  pic: true,
  gnu_symbol_visibility: 'hidden',
  install: false,
)

###############################################################################
# The translator: the port's own code
#
# This is the half that had to change, because it is the only half that knows
# what a PostgreSQL is.  Cloudberry's originals stay where they are, untouched,
# so a merge from apache/cloudberry still applies to them; a behaviour change
# upstream makes there is ported here by hand, which is the rule the plan
# already sets for PostgreSQL files Cloudberry modified.
###############################################################################

orca_transl_inc = include_directories('include', 'compat')

# compat/ is the port's re-implementation of what ORCA asks of a PostgreSQL
# that Cloudberry had patched.  Those functions live in PostgreSQL files
# Cloudberry edited, which the port does not build, so ORCA would otherwise
# have nothing to call.  It is C, not C++: it is server code, and it is the
# translator above it that crosses into C++.
orca_compat_sources = files(
  'compat/clauses.c',
  'compat/lsyscache.c',
  'compat/plancat.c',
  'compat/selfuncs.c',
  'compat/subselect.c',
  'compat/tlist.c',
  'compat/walkers.c',
)

orca_transl_sources = files(
  'gpdbwrappers.cpp',
  'orca_api.cpp',
) + orca_compat_sources

###############################################################################
# What the module links against
###############################################################################

orca_deps = [xerces_dep]
orca_inc = [orca_core_inc, orca_transl_inc]

summary({
  'core sources':      orca_core_sources.length().to_string() + ' files, unmodified',
  'translator sources': orca_transl_sources.length().to_string() + ' files, the port\'s own',
  'xerces-c':          xerces_dep.found() ? 'found' : 'missing',
}, section: 'ORCA')
