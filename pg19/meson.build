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
  # debugoptimized, -O2 -g, is PostgreSQL's own default build type and the
  # one the server is built with (docker/Dockerfile.pg).  Meson's is debug,
  # -O0, which left the modules unoptimized: every ORCA pass of the suites
  # took five to twelve times its planner pass.  Cloudberry builds with -O3
  # (configure.ac, "If the compiler is GCC, then we use -O3").
  default_options: ['c_std=gnu11', 'cpp_std=c++17', 'warning_level=1',
                    'b_ndebug=if-release', 'buildtype=debugoptimized'],
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

# Whether the server checks its assertions (-Dcassert=true), as its
# pg_config.h says: ORCA's own debug checks follow it, unless orca_debug
# says otherwise (orca/meson.build).
pg_cassert = cc.get_define('USE_ASSERT_CHECKING',
                           prefix: '#include "pg_config.h"',
                           include_directories: pg_inc) == '1'
orca_debug = get_option('orca_debug').enabled() or \
             (get_option('orca_debug').auto() and pg_cassert)

# libpq, for the dispatcher.  A backend may not link libpq in code built into
# the server, but an extension may and must: PostgreSQL 19 says so in
# libpq-be-fe.h, and the helpers in libpq-be-fe-helpers.h exist for exactly
# this.  The client headers are not the server's, so they are asked for
# separately.
pg_client_inc = include_directories(pg_includedir)
libpq_dep = cc.find_library('pq', dirs: [pg_libdir], required: true)

# gp_ao's compression: zlib, which PostgreSQL itself needs, and zstd where it
# is installed.
zlib_dep = dependency('zlib', required: true)
zstd_dep = dependency('libzstd', required: false)

# gp_exttable's gpfdist:// and http:// locations.
curl_dep = dependency('libcurl', required: false)

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
#include "access/reloptions.h"
#include "access/tableamext.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "commands/explain.h"
#include "commands/matview.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "replication/syncrep.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/combocid.h"
#include "utils/ruleutils.h"

void		cb_probe(void);

void
cb_probe(void)
{
	TableAmExtRoutine ext = {0};

	new_oid_hook = NULL;					/* R1 */
	combocid_create_hook = NULL;			/* R2 */
	combocid_miss_hook = NULL;				/* R2 */
	SyncRepHoldCancelDuringWait = false;	/* R3 */
	analyze_sample_rows_hook = NULL;		/* O3 */
	explain_node_label_hook = NULL;			/* O4 */
	columnref_fallback_hook = NULL;			/* O10 */
	deparse_function_as_column_hook = NULL;	/* O10 */
	mdunlink_hook = NULL;					/* O22 */
	smgr_file_event_hook = NULL;			/* O21 */
	raw_parser_hook = NULL;					/* O26 */
	star_expansion_filter_hook = NULL;		/* O28 */

	(void) LastNewOidWasPreassigned;					/* R1  */
	(void) XactAdoptCurrentXids;						/* R2  */
	(void) XactAdoptTransactionState;					/* R4  */
	(void) standard_raw_parser;							/* O26 */
	(void) OpenMatViewIncrementalMaintenanceExternal;	/* O27 */
	(void) CloseMatViewIncrementalMaintenanceExternal;	/* O27 */
	(void) ExtensionMarkAdd;							/* O23 */
	(void) RegisterTableAmExtension;					/* O13 */
	(void) table_am_reloptions;							/* O14 */
	(void) table_scan_extractcolumns;					/* O15 */
	ext.index_unique_check = NULL;						/* O16 */
	ext.relation_add_columns = NULL;					/* O17 */
	ext.relation_get_block_sequences = NULL;			/* O18 */
	ext.size_from_am = false;							/* O19 */
	ext.old_row_from_plan = false;						/* O20 */
	(void) ext;
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
    compiles, but does not carry the Cloudberry core patch series (R1-R4, O3,
    O4, O10, O13-O23, O26-O28).  Build PostgreSQL from github/postgres, branch
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

# Cloudberry's global deadlock detector's graph -- its vertices, edges and
# their reduction -- compiled where it lies, unmodified, as gp_task's cron
# parser is: gp_gdd.c supplies the vertices and edges and asks it through the
# include overlay (compat/gdd/).  A vertex is named by Cloudberry's
# DistributedTransactionId, a uint64 its c.h defines and PostgreSQL 19's does
# not, and it asserts with Cloudberry's AssertImply, hence the definitions.
gdd_graph = static_library('gdd_graph',
  files('../src/backend/utils/gdd/gdddetector.c'),
  c_args: mod_c_args + ['-DDistributedTransactionId=uint64',
                        '-DAssertImply(a,b)=Assert(!(a) || (b))'],
  include_directories: [pg_inc],
  pic: true,
)

modules = [
  {
    'name': 'gp_core', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_core/gp_core.c',
                     'modules/gp_core/gp_cluster.c',
                     'modules/gp_core/gp_dispatch.c',
                     'modules/gp_core/gp_dtx.c',
                     'modules/gp_core/gp_fault.c',
                     'modules/gp_core/gp_fts.c',
                     'modules/gp_core/gp_gdd.c',
                     'modules/gp_core/gp_loopback.c',
                     'modules/gp_core/gp_ddl.c',
                     'modules/gp_core/gp_hash.c',
                     'modules/gp_core/gp_legacyhash.c',
                     'modules/gp_core/gp_scan.c',
                     'modules/gp_core/gp_modify.c',
                     'modules/gp_core/gp_explicit.c',
                     'modules/gp_core/gp_analyze.c',
                     'modules/gp_core/gp_motion.c',
                     'modules/gp_core/gp_split.c',
                     'modules/gp_core/gp_segment.c',
                     'modules/gp_core/gp_share.c',
                     'modules/gp_core/gp_ic.c',
                     'modules/gp_core/gp_label.c',
                     'modules/gp_core/gp_catalog.c',
                     'modules/gp_core/gp_median.c',
                     'modules/gp_core/gp_policy.c',
                     'modules/gp_core/gp_settings.c'),
    # The dispatcher is a libpq client of the segments; see gp_dispatch.c.
    'dependencies': [libpq_dep],
    'include_directories': [pg_client_inc],
    # Cloudberry's fault injector, for the tests (gp_fault.c).
    'more_extensions': ['gp_inject_fault'],
    # Cloudberry's global deadlock detector's graph, where it lies.
    'link_with': [gdd_graph],
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
    # Append-optimized tables.  zlib is PostgreSQL's own requirement; zstd,
    # Cloudberry's other compresstype, is used where it is found.
    'name': 'gp_ao', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_ao/gp_ao.c',
                     'modules/gp_ao/ao_am.c',
                     'modules/gp_ao/ao_block.c',
                     'modules/gp_ao/ao_dml.c',
                     'modules/gp_ao/ao_encoding.c',
                     'modules/gp_ao/ao_meta.c',
                     'modules/gp_ao/ao_options.c',
                     'modules/gp_ao/ao_storage.c',
                     'modules/gp_ao/ao_toolkit.c',
                     'modules/gp_ao/ao_vacuum.c',
                     'modules/gp_ao/bitmap/bitmap.c',
                     'modules/gp_ao/bitmap/bitmapattutil.c',
                     'modules/gp_ao/bitmap/bitmapinsert.c',
                     'modules/gp_ao/bitmap/bitmappages.c',
                     'modules/gp_ao/bitmap/bitmapsearch.c',
                     'modules/gp_ao/bitmap/bitmaputil.c',
                     'modules/gp_ao/bitmap/bitmapxlog.c'),
    'include_directories': [include_directories('modules/gp_ao/bitmap')],
    'dependencies': [zlib_dep, zstd_dep],
    'c_args': zstd_dep.found() ? ['-DGP_AO_HAVE_ZSTD'] : [],
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
    # External tables.  fstream reads a file:// location, as it reads
    # gpfdist's (common/fstream/); libcurl, where it is installed, reads and
    # writes gpfdist:// and http:// ones (url_curl.c).
    'name': 'gp_exttable', 'preload': false, 'extension': true, 'build': true,
    'sources': files('modules/gp_exttable/gp_exttable.c',
                     'modules/gp_exttable/copyout.c',
                     'modules/gp_exttable/copysreh.c',
                     'modules/gp_exttable/extaccess.c',
                     'modules/gp_exttable/external.c',
                     'modules/gp_exttable/exttable_ddl.c',
                     'modules/gp_exttable/exttable_fdw.c',
                     'modules/gp_exttable/option.c',
                     'modules/gp_exttable/protocol.c',
                     'modules/gp_exttable/sreh.c',
                     'modules/gp_exttable/uri.c',
                     'modules/gp_exttable/url.c',
                     'modules/gp_exttable/url_curl.c',
                     'modules/gp_exttable/url_custom.c',
                     'modules/gp_exttable/url_execute.c',
                     'modules/gp_exttable/url_file.c',
                     'common/fstream/fstream.c',
                     'common/fstream/gfile.c'),
    'include_directories': [include_directories('common', 'modules/gp_exttable/include')],
    'dependencies': [zlib_dep, zstd_dep, curl_dep],
    'c_args': curl_dep.found() ? ['-DUSE_CURL'] : [],
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
    # Cloudberry's gpcontrib extension of that name, whose functions say how
    # many segments a new table is spread over; gp_sql carries them, since it
    # is what distributes a new table.
    'more_extensions': ['gp_debug_numsegments'],
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
    c_args: mod_c_args + m.get('c_args', []),
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

  # An extension of another name whose functions the module carries.
  foreach extra : m.get('more_extensions', [])
    install_data(
      'modules' / name / (extra + '.control'),
      'modules' / name / (extra + '--1.0.sql'),
      install_dir: pg_extensiondir,
    )
  endforeach

  built_modules += name
  if m['preload']
    preload_modules += name
  endif
endforeach

###############################################################################
# gpfdist
#
# Cloudberry's file server of external tables (src/bin/gpfdist), a program
# of its own that gp_exttable's gpfdist:// locations read from and write to.
# It is built as Cloudberry builds it: against the server's headers, whose
# pg_config.h says whether it speaks SSL and zstd, with the frontend's port
# and common libraries, and with the file streams gp_exttable shares
# (common/fstream), gfile.c as a frontend's.  Its transforms need libyaml.
###############################################################################

apr_dep = dependency('apr-1', required: get_option('gpfdist'))
event_dep = dependency('libevent', required: get_option('gpfdist'))
if apr_dep.found() and event_dep.found()
  yaml_dep = dependency('yaml-0.1', required: false)
  ssl_dep = dependency('openssl', required: false)
  gpfdist_c_args = ['-DFRONTEND', '-DGP_VERSION="' + meson.project_version() + '"']
  gpfdist_sources = files('bin/gpfdist/gpfdist.c',
                          'bin/gpfdist/gpfdist_helper.c',
                          'common/fstream/fstream.c',
                          'common/fstream/gfile.c')
  if yaml_dep.found()
    gpfdist_sources += files('bin/gpfdist/transform.c')
    gpfdist_c_args += ['-DGPFXDIST']
  endif
  executable('gpfdist', gpfdist_sources,
    c_args: gpfdist_c_args,
    include_directories: [pg_inc, include_directories('common', 'bin/gpfdist')],
    dependencies: [apr_dep, event_dep, yaml_dep, ssl_dep, zlib_dep, zstd_dep,
                   dependency('threads')],
    link_args: [pg_libdir / 'libpgcommon.a', pg_libdir / 'libpgport.a', '-lm'],
    install: true,
    install_dir: pg['bindir'],
  )
  built_modules += 'gpfdist (program)'
endif

###############################################################################
# Test-only modules
#
# gp_probe sets every hook of the core patch series so that the hook tests can
# check it is called, and gp_storage_probe is a storage handler.  Neither is
# part of the port: they are built and installed only where the tests run.
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

  # A storage handler for the storage and cluster suites: a directory stands
  # for the store, and credentials a user mapping gives are asked for.
  shared_module('gp_storage_probe', files('test/storage/gp_storage_probe.c'),
    c_args: mod_c_args,
    link_args: mod_link_args,
    include_directories: [pg_inc, cb_inc],
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  install_data(
    'test/storage/gp_storage_probe.control',
    'test/storage/gp_storage_probe--1.0.sql',
    install_dir: pg_extensiondir,
  )
  built_modules += 'gp_storage_probe (test only)'

  # Cloudberry's demonstration protocol, which its external table tests
  # make a protocol of (contrib/extprotocol), against gp_exttable's API.
  shared_module('gpextprotocol', files('test/exttable/gpextprotocol.c'),
    c_args: mod_c_args,
    link_args: mod_link_args,
    include_directories: [pg_inc, cb_inc,
                          include_directories('modules/gp_exttable/include')],
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  built_modules += 'gpextprotocol (test only)'

  # And its demonstration formatter (contrib/formatter), which the custom
  # format's test reads a table with.
  shared_module('gpformatter', files('test/exttable/gpformatter.c'),
    c_args: mod_c_args,
    link_args: mod_link_args,
    include_directories: [pg_inc, cb_inc,
                          include_directories('modules/gp_exttable/include')],
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  built_modules += 'gpformatter (test only)'
endif

subdir('test')

summary({
  'pg_config':    pg_config.full_path(),
  'PostgreSQL':   pg['version'],
  'core patches': have_patched_pg,
  'assertions':   pg_cassert,
  'pkglibdir':    pg_pkglibdir,
  'extensiondir': pg_extensiondir,
}, section: 'PostgreSQL')

summary({
  'built':   ', '.join(built_modules),
  'preload': ', '.join(preload_modules),
  'build type': get_option('buildtype'),
  'ORCA debug checks (GPOS_DEBUG)': get_option('orca') and orca_debug,
}, section: 'Modules')
