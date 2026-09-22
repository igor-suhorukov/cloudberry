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

# Apache Cloudberry on PostgreSQL 19 -- the port's modules.
#
# This is a stand-alone project: it builds against an installed, patched
# PostgreSQL 19, found through pg_config, and never against src/include of
# this tree, which holds PostgreSQL 16 headers.

project('cloudberry-pg19', 'c',
  version: '3.0.0-devel',
  license: 'Apache-2.0',
  meson_version: '>= 0.57',
  # gnu11, not c11: PostgreSQL's headers use POSIX declarations such as
  # sigjmp_buf and clock_gettime, which strict ISO mode hides.
  # c++17 is what ORCA is written against.
  default_options: ['c_std=gnu11', 'cpp_std=c++17', 'warning_level=1',
                    'b_ndebug=if-release'],
)

cc = meson.get_compiler('c')

# ORCA is C++.  Ask for the language only when it is being built, so that a
# build with -Dorca=false needs no C++ compiler at all.
if get_option('orca')
  add_languages('cpp', native: false, required: true)
  cpp = meson.get_compiler('cpp')
endif

###############################################################################
# The PostgreSQL we build against
###############################################################################

pg_config = find_program(get_option('pg_config'), required: true)

pg_query = ['--includedir-server', '--includedir', '--libdir', '--pkglibdir',
            '--sharedir', '--bindir', '--version']
pg = {}
foreach opt : pg_query
  res = run_command(pg_config, opt, check: true)
  pg += {opt.substring(2): res.stdout().strip()}
endforeach

pg_includedir_server = pg['includedir-server']
pg_includedir        = pg['includedir']
pg_libdir            = pg['libdir']
pg_pkglibdir         = pg['pkglibdir']
pg_sharedir          = pg['sharedir']
pg_extensiondir      = pg_sharedir / 'extension'

# "PostgreSQL 19devel" / "PostgreSQL 19.1" -> 19
pg_major = pg['version'].split(' ')[1].split('.')[0].split('devel')[0].split('beta')[0].split('rc')[0]
if pg_major.to_int() < 19
  error('this port needs PostgreSQL 19 or later, but @0@ reports "@1@"'.format(
        pg_config.full_path(), pg['version']))
endif

pg_inc = include_directories(pg_includedir_server)

# libpq, for the dispatcher.  A backend may not link libpq in code built into
# the server, but an extension may and must: PostgreSQL 19 says so in
# libpq-be-fe.h, and the helpers in libpq-be-fe-helpers.h exist for exactly
# this.  The client headers are not the server's, so they are asked for
# separately.
pg_client_inc = include_directories(pg_includedir)
libpq_dep = cc.find_library('pq', dirs: [pg_libdir], required: true)

# PostgreSQL leaves extension symbols to be resolved at load time, and opens
# libraries with RTLD_GLOBAL, which is how the modules listed after gp_core
# reach into it.
# PostgreSQL's own warning set.  Not -Wextra: PG_FUNCTION_ARGS names a
# parameter that most functions never use, so -Wunused-parameter is noise here.
mod_c_args = ['-DGP_VERSION="' + meson.project_version() + '"']
mod_c_args += cc.get_supported_arguments(
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Wendif-labels',
  '-Wmissing-format-attribute',
  '-Wcast-function-type',
  '-Wshadow=compatible-local',
  '-Wformat-security',
  '-Wimplicit-fallthrough=3',
  '-Wdeclaration-after-statement',
)
mod_link_args = []
if host_machine.system() == 'darwin'
  mod_link_args += ['-bundle', '-undefined', 'dynamic_lookup']
endif

###############################################################################
# Is this the patched PostgreSQL?
#
# Every module is written against the core patch series, so a build against an
# unpatched server would fail later with an unresolved symbol at load time.
# Say so now instead, and name the branch that carries the patches.
###############################################################################

patch_probe = '''
#include "postgres.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "commands/explain.h"
#include "commands/matview.h"
#include "commands/vacuum.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "replication/syncrep.h"
#include "storage/md.h"
#include "utils/combocid.h"

void		cb_probe(void);

void
cb_probe(void)
{
	new_oid_hook = NULL;					/* R1 */
	combocid_create_hook = NULL;			/* R2 */
	combocid_miss_hook = NULL;				/* R2 */
	SyncRepHoldCancelDuringWait = false;	/* R3 */
	analyze_sample_rows_hook = NULL;		/* O3 */
	explain_node_label_hook = NULL;			/* O4 */
	mdunlink_hook = NULL;					/* O22 */
	raw_parser_hook = NULL;					/* O26 */
	star_expansion_filter_hook = NULL;		/* O28 */

	(void) LastNewOidWasPreassigned;					/* R1  */
	(void) XactAdoptCurrentXids;						/* R2  */
	(void) standard_raw_parser;							/* O26 */
	(void) OpenMatViewIncrementalMaintenanceExternal;	/* O27 */
	(void) CloseMatViewIncrementalMaintenanceExternal;	/* O27 */
}
'''

# Two probes, so that a failure says which of the two things went wrong.
if not cc.compiles('#include "postgres.h"\nvoid cb_probe(void); void cb_probe(void) {}',
                   include_directories: pg_inc,
                   name: 'PostgreSQL server headers compile')
  error('''
    cannot compile against the server headers of
    @0@
    See meson-logs/meson-log.txt for the compiler output.  This is a problem
    with the toolchain or that installation, not with the port.'''.format(
    pg_config.full_path()))
endif

have_patched_pg = cc.compiles(patch_probe,
  include_directories: pg_inc,
  name: 'PostgreSQL carries the Cloudberry core patch series')

if not have_patched_pg and get_option('require_patched_pg')
  error('''
    @0@
    compiles, but does not carry the Cloudberry core patch series (R1-R3, O3,
    O4, O22, O26-O28).  Build PostgreSQL from github/postgres, branch
    REL_19_STABLE_CLOUDBERRY, and point -Dpg_config at it.
    Pass -Drequire_patched_pg=false to configure anyway.'''.format(
    pg_config.full_path()))
endif

###############################################################################
# ORCA
#
# Built before the module table, because gp_orca's entry names what comes out
# of it: orca_core (Cloudberry's ORCA, unmodified), orca_transl_sources (the
# translator, which is the port's own code), and the include paths and
# xerces-c that both need.  See orca/meson.build.
###############################################################################

orca_core = []
orca_deps = []
orca_inc = []
orca_transl_sources = []
orca_cpp_args = []

if get_option('orca')
  subdir('orca')
endif

###############################################################################
# The modules
#
#   preload   this module may only be loaded through shared_preload_libraries
#   extension it has a control file and an SQL script, so CREATE EXTENSION
#             gives users its objects
#   build     whether to build it at all
#
# Sources are listed by their path in this tree.  The milestones add the
# Cloudberry sources that each module is made of; see cloudberry.md,
# "Extension modules and where their code comes from".
###############################################################################

modules = [
  {
    'name': 'gp_core', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_core/gp_core.c',
                     'modules/gp_core/gp_cluster.c',
                     'modules/gp_core/gp_dispatch.c',
                     'modules/gp_core/gp_ddl.c',
                     'modules/gp_core/gp_hash.c',
                     'modules/gp_core/gp_scan.c',
                     'modules/gp_core/gp_modify.c',
                     'modules/gp_core/gp_analyze.c',
                     'modules/gp_core/gp_label.c',
                     'modules/gp_core/gp_median.c',
                     'modules/gp_core/gp_policy.c'),
    # The dispatcher is a libpq client of the segments; see gp_dispatch.c.
    'dependencies': [libpq_dep],
    'include_directories': [pg_client_inc],
  },
  {
    'name': 'interconnect', 'preload': true, 'extension': false, 'build': true,
    'sources': files('modules/interconnect/interconnect.c'),
  },
  {
    'name': 'udp2', 'preload': true, 'extension': false, 'build': get_option('udp2'),
    'sources': files('modules/udp2/udp2.c'),
  },
  {
    # ORCA, which is C++: the module is its four core libraries from
    # Cloudberry's tree, unmodified, plus the translator under orca/, which is
    # the only part that knows about PostgreSQL and so the only part the port
    # had to write.
    'name': 'gp_orca', 'preload': true, 'extension': true, 'build': get_option('orca'),
    'sources': files('modules/gp_orca/gp_orca.c',
                     'modules/gp_orca/gp_orca_planner.c') + orca_transl_sources,
    'cpp_args': orca_cpp_args,
    'link_with': orca_core,
    'dependencies': orca_deps,
    'include_directories': orca_inc,
  },
  {
    'name': 'gp_ao', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_ao/gp_ao.c'),
  },
  {
    'name': 'pax', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/pax/pax.c'),
  },
  {
    'name': 'gp_tde', 'preload': true, 'extension': true, 'build': get_option('tde'),
    'sources': files('modules/gp_tde/gp_tde.c'),
  },
  {
    'name': 'gp_exttable', 'preload': false, 'extension': true, 'build': true,
    'sources': files('modules/gp_exttable/gp_exttable.c'),
  },
  {
    'name': 'gp_resource', 'preload': false, 'extension': true, 'build': true,
    'sources': files('modules/gp_resource/gp_resource.c'),
  },
  {
    # Preloaded: it registers a background worker and the authentication hook,
    # and neither can be done once the postmaster is running.
    'name': 'gp_security', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_security/gp_security.c',
                     'modules/gp_security/profile.c',
                     'modules/gp_security/login.c',
                     'modules/gp_security/password.c'),
  },
  {
    # Preloaded: a background worker can only be registered while libraries
    # are being preloaded, so a scheduler that arrived through CREATE
    # EXTENSION would have nothing running its jobs.
    'name': 'gp_task', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_task/gp_task.c',
                     'modules/gp_task/task_jobs.c',
                     'modules/gp_task/task_worker.c',
                     'modules/gp_task/task_vixie.c',
                     # Cloudberry's cron expression parser, compiled where it
                     # lies: Paul Vixie's cron, under its own licence, reached
                     # through the include overlay in compat/task/.
                     '../src/backend/task/entry.c',
                     '../src/backend/task/misc.c'),
  },
  {
    'name': 'gp_matview', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_matview/gp_matview.c',
                     'modules/gp_matview/dynamic.c',
                     'modules/gp_matview/ivm_create.c',
                     'modules/gp_matview/ivm_delta.c',
                     'modules/gp_matview/ivm_maintain.c',
                     'modules/gp_matview/ivm_state.c'),
  },
  {
    # Preloaded: it reads CREATE TABLE and its friends before PostgreSQL does,
    # and registers a security label provider, which every backend that reads
    # such a label needs registered.
    'name': 'gp_sql', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_sql/gp_sql.c',
                     'modules/gp_sql/funcattr.c',
                     'modules/gp_sql/tag.c',
                     'modules/gp_sql/dirtable.c',
                     'modules/gp_sql/storage.c',
                     'modules/gp_sql/distribution.c',
                     # O26's desugaring: Cloudberry's spelling of a statement
                     # rewritten into PostgreSQL's, before the grammar sees it,
                     # and the parse tree's positions put back in the user's.
                     'grammar/gp_desugar.c',
                     'grammar/gp_parseloc.c',
                     # Cloudberry's classic partition clauses: the parser the
                     # rewrite checks them with, and gp_sql's side, which
                     # makes the partitions they describe.
                     'grammar/gp_partition.c',
                     'modules/gp_sql/partition.c'),
  },
]

cb_inc = include_directories('include', 'compat', 'grammar')

built_modules = []
preload_modules = []

foreach m : modules
  if not m['build']
    continue
  endif

  name = m['name']

  shared_module(name, m['sources'],
    c_args: mod_c_args,
    cpp_args: m.get('cpp_args', []),
    link_args: mod_link_args,
    # As PGXS does, so that a module which links libpq finds it at load time
    # without anyone setting LD_LIBRARY_PATH.  install_rpath and not a linker
    # flag: meson strips the run path it finds when it installs, and the
    # server that loaded gp_core from the build directory could not load the
    # installed copy.
    build_rpath: pg_libdir,
    install_rpath: pg_libdir,
    link_with: m.get('link_with', []),
    dependencies: m.get('dependencies', []),
    include_directories: [pg_inc, cb_inc] + m.get('include_directories', []),
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )

  if m['extension']
    install_data(
      'modules' / name / (name + '.control'),
      'modules' / name / (name + '--1.0.sql'),
      install_dir: pg_extensiondir,
    )
  endif

  built_modules += name
  if m['preload']
    preload_modules += name
  endif
endforeach

###############################################################################
# Test-only modules
#
# gp_probe sets every hook of the core patch series so that the hook tests can
# check it is called.  It is not part of the port: it is built and installed
# only where those tests run.
###############################################################################

if get_option('hook_tests')
  shared_module('gp_probe', files('test/hooks/gp_probe.c'),
    c_args: mod_c_args,
    link_args: mod_link_args,
    include_directories: [pg_inc, cb_inc],
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  install_data(
    'test/hooks/gp_probe.control',
    'test/hooks/gp_probe--1.0.sql',
    install_dir: pg_extensiondir,
  )
  built_modules += 'gp_probe (test only)'
endif

subdir('test')

summary({
  'pg_config':    pg_config.full_path(),
  'PostgreSQL':   pg['version'],
  'core patches': have_patched_pg,
  'pkglibdir':    pg_pkglibdir,
  'extensiondir': pg_extensiondir,
}, section: 'PostgreSQL')

summary({
  'built':   ', '.join(built_modules),
  'preload': ', '.join(preload_modules),
}, section: 'Modules')
