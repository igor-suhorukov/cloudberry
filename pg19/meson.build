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
fs = import('fs')

# ORCA, PAX, gpcloud and UDP2 are C++.  Ask for the language only when one of
# them is being built, so that a build without them needs no C++ compiler at
# all.
if get_option('orca') or get_option('pax') or get_option('gpcloud') or get_option('udp2')
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
#include "commands/matview.h"
#include "commands/tablespace.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "replication/walsender.h"
#include "storage/smgr.h"
#include "utils/combocid.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"

void		cb_probe(void);

void
cb_probe(void)
{
	TableAmExtRoutine ext = {0};

	new_oid_hook = NULL;					/* R1 */
	combocid_create_hook = NULL;			/* R2 */
	combocid_miss_hook = NULL;				/* R2 */
	analyze_sample_rows_hook = NULL;		/* O3 */
	columnref_fallback_hook = NULL;			/* O10 */
	deparse_function_as_column_hook = NULL;	/* O10 */
	smgr_file_event_hook = NULL;			/* O21 */
	memory_block_alloc_hook = NULL;			/* O25 */
	raw_parser_hook = NULL;					/* O26 */
	star_expansion_filter_hook = NULL;		/* O28 */
	tablespace_location_hook = NULL;		/* O32 */
	tablespace_location_drop_hook = NULL;	/* O32 */
	xact_commit_recorded_hook = NULL;		/* O33 */
	physical_slot_restart_lsn_hook = NULL;	/* O36 */
	XactAbortAgainAfterEnd = false;			/* O37 */

	(void) LastNewOidWasPreassigned;					/* R1  */
	(void) XactAdoptTransactionState;					/* R4  */
	(void) standard_raw_parser;							/* O26 */
	(void) OpenMatViewIncrementalMaintenanceExternal;	/* O27 */
	(void) CloseMatViewIncrementalMaintenanceExternal;	/* O27 */
	(void) RegisterTableAmExtension;					/* O13 */
	(void) table_am_reloptions;							/* O14 */
	(void) table_scan_extractcolumns;					/* O15 */
	ext.index_unique_check = NULL;						/* O16 */
	ext.relation_add_columns = NULL;					/* O17 */
	ext.relation_get_block_sequences = NULL;			/* O18 */
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
    compiles, but does not carry the Cloudberry core patch series (R1, R2, R4,
    O3, O10, O13-O18, O21, O25-O28, O32, O33, O36, O37).  Build PostgreSQL from
    github/postgres, branch
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
# PAX
#
# Built before the module table too: the pax module's entry is what comes out
# of it -- Cloudberry's contrib/pax_storage, compiled where it is but for the
# port's copies of the files that had to change.  See pax/meson.build.
###############################################################################

pax_sources = []
pax_cpp_args = []
pax_deps = []
pax_inc = []
pax_gtest_sources = []
pax_gtest_inc = []

if get_option('pax')
  subdir('pax')
endif

###############################################################################
# UDP2
#
# Its C++ core, Cloudberry's contrib/udp2/ic_common, compiled where it lies
# into a static library, which the udp2 module's entry links: see
# udp2/meson.build.
###############################################################################

udp2_core = []
udp2_inc = []

if get_option('udp2')
  subdir('udp2')
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

# Cloudberry's memory protection -- its vmem tracker, red zone handler,
# runaway cleaner, idle tracker, event versions and session states --
# compiled where it lies, unmodified: gp_resource counts the memory contexts'
# blocks through O25 (memprot.c) and asks these through the include overlay
# (compat/memprot/), which answers what they ask of Cloudberry's globals in
# the port's terms.  Their C is Cloudberry's style: declarations among
# statements, and functions declared in the files that call them.
gp_memprot = static_library('gp_memprot',
  files('../src/backend/utils/mmgr/vmem_tracker.c',
        '../src/backend/utils/mmgr/redzone_handler.c',
        '../src/backend/utils/mmgr/runaway_cleaner.c',
        '../src/backend/utils/mmgr/idle_tracker.c',
        '../src/backend/utils/mmgr/event_version.c',
        '../src/backend/utils/session_state.c'),
  c_args: mod_c_args + ['-Wno-declaration-after-statement',
                        '-Wno-missing-prototypes'],
  include_directories: [include_directories('compat/memprot', 'include'), pg_inc],
  pic: true,
)

# Cloudberry's cgroup code -- the cgroup file system of either version, a
# resource group's cgroup, and the io_limit grammar -- compiled where it lies,
# unmodified, for gp_resource's groups, which drive it through its table of
# operations (resgroup.c), as Cloudberry's resgroup.c does; it reads
# Cloudberry's resource manager through the include overlay
# (compat/resgroup/).  It is Linux's, as Cloudberry's is.  GP_VERSION_NUM is
# the project's version, as Cloudberry's configure makes it: it keeps the
# cpuset controller optional.
if host_machine.system() == 'linux'
  bison = find_program('bison', required: true)
  flex = find_program('flex', required: true)
  cgroup_src = '../src/backend/utils/resgroup/'
  io_limit_gram = custom_target('io_limit_gram',
    input: cgroup_src + 'io_limit_gram.y',
    output: ['io_limit_gram.c', 'io_limit_gram.h'],
    command: [bison, '-d', '-Wno-deprecated', '-o', '@OUTPUT0@', '@INPUT@'],
  )
  io_limit_scanner = custom_target('io_limit_scanner',
    input: cgroup_src + 'io_limit_scanner.l',
    output: 'io_limit_scanner.c',
    command: [flex, '-CF', '-p', '-p', '-o', '@OUTPUT@', '@INPUT@'],
  )
  gp_version = meson.project_version().split('-')[0].split('.')
  gp_version_num = gp_version[0].to_int() * 10000 + gp_version[1].to_int() * 100 + gp_version[2].to_int()
  gp_cgroup = static_library('gp_cgroup',
    files(cgroup_src + 'cgroup.c',
          cgroup_src + 'cgroup-ops-linux-v1.c',
          cgroup_src + 'cgroup-ops-linux-v2.c',
          cgroup_src + 'cgroup_io_limit.c'),
    io_limit_gram, io_limit_scanner,
    c_args: mod_c_args + ['-D_GNU_SOURCE', '-DGP_VERSION_NUM=' + gp_version_num.to_string()] +
      cc.get_supported_arguments('-Wno-declaration-after-statement',
                                 '-Wno-missing-prototypes',
                                 '-Wno-shadow=compatible-local',
                                 '-Wno-unused-variable',
                                 '-Wno-unused-function'),
    include_directories: [include_directories('compat/resgroup', 'modules/gp_resource', 'include'), pg_inc],
    pic: true,
  )
else
  gp_cgroup = static_library('gp_cgroup',
    files('../src/backend/utils/resgroup/cgroup-ops-dummy.c'),
    c_args: mod_c_args,
    include_directories: [include_directories('compat/resgroup', 'modules/gp_resource', 'include'), pg_inc],
    pic: true,
  )
endif

# The interconnect proxy's server, Cloudberry's contrib/interconnect/proxy,
# compiled where it lies, unmodified, for the interconnect module's proxy
# transport: a background worker on each node that carries every pair of
# nodes' Motions over one connection.  All of it but its backend half
# (ic_proxy_backend.c, which is ic-tcp's), whose place the module's
# proxy_backend.c takes; what it asks of Cloudberry's globals it reads through
# the include overlay (compat/ic_proxy/).  It is written on libuv, and built
# where libuv is.
libuv_dep = dependency('libuv', required: get_option('ic_proxy'))
ic_proxy_server = []
ic_proxy_inc = []
if libuv_dep.found()
  ic_proxy_src = '../contrib/interconnect/proxy/'
  ic_proxy_inc = include_directories('compat/ic_proxy', '../contrib/interconnect/proxy')
  ic_proxy_server = static_library('ic_proxy_server',
    files(ic_proxy_src + 'ic_proxy_addr.c',
          ic_proxy_src + 'ic_proxy_client.c',
          ic_proxy_src + 'ic_proxy_iobuf.c',
          ic_proxy_src + 'ic_proxy_key.c',
          ic_proxy_src + 'ic_proxy_main.c',
          ic_proxy_src + 'ic_proxy_packet.c',
          ic_proxy_src + 'ic_proxy_peer.c',
          ic_proxy_src + 'ic_proxy_pkt_cache.c',
          ic_proxy_src + 'ic_proxy_router.c'),
    c_args: mod_c_args + cc.get_supported_arguments('-Wno-declaration-after-statement',
                                                    '-Wno-missing-prototypes',
                                                    '-Wno-shadow=compatible-local'),
    include_directories: [ic_proxy_inc, include_directories('include'), pg_inc],
    dependencies: libuv_dep,
    gnu_symbol_visibility: 'hidden',
    pic: true,
  )
endif

modules = [
  {
    'name': 'gp_core', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_core/gp_core.c',
                     'modules/gp_core/gp_cluster.c',
                     'modules/gp_core/gp_dispatch.c',
                     'modules/gp_core/gp_dtx.c',
                     'modules/gp_core/gp_fault.c',
                     'modules/gp_core/gp_dbcopy.c',
                     'modules/gp_core/gp_extmark.c',
                     'modules/gp_core/gp_fts.c',
                     'modules/gp_core/gp_standby.c',
                     'modules/gp_core/gp_gdd.c',
                     'modules/gp_core/gp_loopback.c',
                     'modules/gp_core/gp_ddl.c',
                     'modules/gp_core/gp_refresh.c',
                     'modules/gp_core/gp_hash.c',
                     'modules/gp_core/gp_legacyhash.c',
                     'modules/gp_core/gp_scan.c',
                     'modules/gp_core/gp_modify.c',
                     'modules/gp_core/gp_subselect.c',
                     'modules/gp_core/gp_explicit.c',
                     'modules/gp_core/gp_analyze.c',
                     'modules/gp_core/gp_motion.c',
                     'modules/gp_core/gp_split.c',
                     'modules/gp_core/gp_seq.c',
                     'modules/gp_core/gp_segment.c',
                     'modules/gp_core/gp_segadmin.c',
                     'modules/gp_core/gp_size.c',
                     'modules/gp_core/gp_share.c',
                     'modules/gp_core/gp_record.c',
                     'modules/gp_core/gp_metatrack.c',
                     'modules/gp_core/gp_ic.c',
                     'modules/gp_core/gp_label.c',
                     'modules/gp_core/gp_catalog.c',
                     'modules/gp_core/gp_median.c',
                     'modules/gp_core/gp_analytic.c',
                     'modules/gp_core/gp_monitor.c',
                     'modules/gp_core/gp_policy.c',
                     'modules/gp_core/gp_settings.c',
                     'modules/gp_core/gp_explain.c',
                     'modules/gp_core/gp_dtm_debug.c',
                     'modules/gp_core/gp_metrics.c',
                     'modules/gp_core/gp_log.c',
                     'modules/gp_core/gp_toolkit.c',
                     'modules/gp_core/gp_partmaint.c',
                     'modules/gp_core/gp_workfile.c',
                     'modules/gp_core/gp_partanalyze.c',
                     'modules/gp_core/gp_hll.c',
                     'modules/gp_core/gp_partmerge.c',
                     'modules/gp_core/gp_rtfilter.c',
                     'modules/gp_core/gp_foreign.c',
                     'modules/gp_core/gp_expand.c',
                     'modules/gp_core/gp_endpoint.c',
                     'modules/gp_core/gp_parallel.c',
                     'modules/gp_core/gp_dirxact.c',
                     'modules/gp_core/gp_utilmode.c',
                     'modules/gp_core/gp_signal.c',
                     'modules/gp_core/gp_builtins.c'),
    # The dispatcher is a libpq client of the segments; see gp_dispatch.c.
    'dependencies': [libpq_dep],
    'include_directories': [pg_client_inc],
    # Cloudberry's fault injector, for the tests (gp_fault.c).
    'more_extensions': ['gp_inject_fault'],
    # Cloudberry's global deadlock detector's graph, where it lies.
    'link_with': [gdd_graph],
  },
  {
    # The proxy transport, where libuv is (ic_proxy_server, above).
    'name': 'interconnect', 'preload': true, 'extension': false, 'build': true,
    'sources': files('modules/interconnect/interconnect.c') +
               (libuv_dep.found() ? files('modules/interconnect/proxy_backend.c') : []),
    'c_args': libuv_dep.found() ? ['-DUSE_IC_PROXY'] : [],
    'link_with': ic_proxy_server,
    'dependencies': libuv_dep.found() ? [libuv_dep] : [],
    'include_directories': ic_proxy_inc,
  },
  {
    # Cloudberry's UDP2 transport: its C++ core, unmodified (udp2/), under
    # the port's adapter; it runs a thread of its own in each backend that
    # uses it.
    'name': 'udp2', 'preload': true, 'extension': false, 'build': get_option('udp2'),
    'sources': files('modules/udp2/udp2.c', 'modules/udp2/udp2_interrupt.cpp'),
    'link_with': udp2_core,
    'include_directories': udp2_inc,
    'dependencies': [dependency('threads')],
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
                     'modules/gp_ao/ao_batch.c',
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
                     'modules/gp_ao/bitmap/bitmapinspect.c',
                     'modules/gp_ao/bitmap/bitmappages.c',
                     'modules/gp_ao/bitmap/bitmapsearch.c',
                     'modules/gp_ao/bitmap/bitmaputil.c',
                     'modules/gp_ao/bitmap/bitmapxlog.c'),
    'include_directories': [include_directories('modules/gp_ao/bitmap')],
    'dependencies': [zlib_dep, zstd_dep],
    'c_args': zstd_dep.found() ? ['-DGP_AO_HAVE_ZSTD'] : [],
  },
  {
    # PAX, which is C++: Cloudberry's contrib/pax_storage and the port's
    # copies of the files of it that had to change (pax/meson.build).
    'name': 'pax', 'preload': true, 'extension': true, 'build': get_option('pax'),
    'sources': files('modules/pax/pax.c', 'modules/pax/pax_catalog.c',
                     'modules/pax/pax_cbcore.c',
                     'modules/pax/pax_numeric.c') + pax_sources,
    'cpp_args': pax_cpp_args,
    'dependencies': pax_deps,
    'include_directories': pax_inc,
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
    # Preloaded: shared memory for the queues and the groups, the hooks every
    # statement goes through, and a label provider every backend that reads a
    # definition needs.
    'name': 'gp_resource', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_resource/gp_resource.c',
                     'modules/gp_resource/resdefs.c',
                     'modules/gp_resource/resqueue.c',
                     'modules/gp_resource/resgroup.c',
                     'modules/gp_resource/memprot.c'),
    'link_with': [gp_cgroup, gp_memprot],
    'include_directories': [include_directories('compat/resgroup')],
  },
  {
    # Preloaded: it registers a background worker and the authentication hook,
    # and neither can be done once the postmaster is running.
    'name': 'gp_security', 'preload': true, 'extension': true, 'build': true,
    'sources': files('modules/gp_security/gp_security.c',
                     'modules/gp_security/profile.c',
                     'modules/gp_security/login.c',
                     'modules/gp_security/password.c',
                     'modules/gp_security/deny.c'),
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
                     'modules/gp_matview/ivm_state.c',
                     'modules/gp_matview/ivm_cluster.c',
                     'modules/gp_matview/mvaux.c',
                     'modules/gp_matview/aqumv.c'),
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
                     'modules/gp_sql/extscript.c',
                     # O26's desugaring: Cloudberry's spelling of a statement
                     # rewritten into PostgreSQL's, before the grammar sees it,
                     # and the parse tree's positions put back in the user's.
                     'grammar/gp_desugar.c',
                     'grammar/gp_parseloc.c',
                     # Cloudberry's classic partition clauses: the parser the
                     # rewrite checks them with, and gp_sql's side, which
                     # makes the partitions they describe.
                     'grammar/gp_partition.c',
                     'modules/gp_sql/partition.c',
                     # Cloudberry's COPY of a directory table's file.
                     'modules/gp_sql/dircopy.c'),
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
# gp_instrument_shmem
#
# Cloudberry's gpcontrib library of that name, which reads the slots of
# gp_core's query metrics (gp_metrics.c): a library alone, as Cloudberry's
# is, whose functions its users make themselves.
###############################################################################

shared_module('gp_instrument_shmem', files('modules/gp_core/gp_instrument_shmem.c'),
  c_args: mod_c_args,
  link_args: mod_link_args,
  include_directories: [pg_inc, cb_inc],
  name_prefix: '',
  install: true,
  install_dir: pg_pkglibdir,
)
built_modules += 'gp_instrument_shmem'

###############################################################################
# diskquota
#
# Cloudberry's gpcontrib/diskquota, whose library is named for its version,
# diskquota-2.3, as Cloudberry names it: that is the name its extension's
# script and shared_preload_libraries give.  Only 2.3 is built, so its
# extension has only 2.3's script: 2.2's names a library of its own.
###############################################################################

diskquota_version = '2.3.0'
diskquota_binary = 'diskquota-2.3'
shared_module(diskquota_binary,
  files('modules/diskquota/diskquota.c',
        'modules/diskquota/diskquota_port.c',
        'modules/diskquota/diskquota_utility.c',
        'modules/diskquota/enforcement.c',
        'modules/diskquota/gp_activetable.c',
        'modules/diskquota/monitored_db.c',
        'modules/diskquota/quotamodel.c',
        'modules/diskquota/relation_cache.c'),
  # Cloudberry's sources as they are, whose declarations follow statements
  # throughout; and built as Cloudberry's tree builds them, CMake's Debug
  # build, whose DISKQUOTA_DEBUG lets diskquota.naptime be 0, as the tests
  # set it.
  c_args: mod_c_args + ['-Wno-declaration-after-statement',
                        '-DDISKQUOTA_DEBUG=1',
                        '-DDISKQUOTA_VERSION="' + diskquota_version + '"',
                        '-DDISKQUOTA_MAJOR_VERSION=2',
                        '-DDISKQUOTA_MINOR_VERSION=3',
                        '-DDISKQUOTA_PATCH_VERSION=0',
                        '-DDISKQUOTA_BINARY_NAME="' + diskquota_binary + '"'],
  link_args: mod_link_args,
  include_directories: [pg_inc, cb_inc],
  name_prefix: '',
  install: true,
  install_dir: pg_pkglibdir,
)
install_data(
  'modules/diskquota/diskquota.control',
  'modules/diskquota/diskquota--2.3.sql',
  install_dir: pg_extensiondir,
)
built_modules += diskquota_binary

###############################################################################
# pg_hint_plan
#
# Cloudberry's gpcontrib/pg_hint_plan, 1.3.9, for PostgreSQL 19: its join
# search is core.c, PostgreSQL 19's copied by gen_core.py, and ORCA reads its
# hints through gp_orca's plan_hint_hook (pg_hint_plan.c's head says what
# changed from Cloudberry's).  Nothing preloads it: a session LOADs it, as
# the tests do.  The extension, whose table the hint table setting reads, is
# Cloudberry's scripts as they are.
###############################################################################

shared_module('pg_hint_plan',
  files('modules/pg_hint_plan/pg_hint_plan.c'),
  c_args: mod_c_args,
  link_args: mod_link_args,
  include_directories: [pg_inc],
  name_prefix: '',
  install: true,
  install_dir: pg_pkglibdir,
)
pg_hint_plan_scripts = ['pg_hint_plan.control', 'pg_hint_plan--1.3.0.sql']
foreach v : ['0', '1', '2', '3', '4', '5', '6', '7', '8']
  pg_hint_plan_scripts += 'pg_hint_plan--1.3.@0@--1.3.@1@.sql'.format(v, v.to_int() + 1)
endforeach
foreach f : pg_hint_plan_scripts
  install_data('../gpcontrib/pg_hint_plan' / f, install_dir: pg_extensiondir)
endforeach
built_modules += 'pg_hint_plan'

###############################################################################
# datalake_fdw
#
# Cloudberry's contrib/datalake_fdw: the Iceberg table access method and its
# catalog and volume foreign-data wrappers, on ProcessUtility_hook and
# object_access_hook, preloaded after gp_core and gp_sql.  Its files are
# compiled where they lie, but for the port's copies of the ones PostgreSQL
# 19 or the port changed, under modules/datalake_fdw/src at the same paths,
# which come first on the include path and so shadow Cloudberry's headers
# too, as PAX's copies do; each says at its head what changed.  Three of its
# files are C++, which includes no header of PostgreSQL's.  Its symbols stay
# its own but for what PostgreSQL calls, which PGDLLEXPORT marks, as
# Cloudberry's export list has them (exports.txt).
###############################################################################

add_languages('cpp', native: false, required: true)
datalake_root = '../contrib/datalake_fdw'
datalake_sources = []
foreach path : ['src/am_iceberg/pg_iceberg_am_handler.c',
                'src/am_iceberg/pg_iceberg_extensible.c',
                'src/am_iceberg/pg_iceberg_ddl.c',
                'src/am_iceberg/pg_iceberg_options.c',
                'src/am_iceberg/pg_iceberg_guc.c',
                'src/am_iceberg/pg_iceberg_reject.c',
                'src/iceberg_catalog_fdw/iceberg_catalog_fdw.c',
                'src/iceberg_catalog_fdw/iceberg_catalog_option.c',
                'src/iceberg_volume_fdw/iceberg_volume_fdw.c',
                'src/iceberg_volume_fdw/iceberg_volume_option.c',
                'src/meta/meta_engine_registry.c',
                'src/meta/meta_engine_init.c',
                'src/meta/engine_stub/stub_engine.c',
                'src/format/format_registry.c',
                'src/common/dl_err.c',
                'src/common/dl_option_util.c',
                'src/common/parser_option.c',
                'src/common/file_system_wrapper.cpp',
                'src/common/s3_file_system.cpp',
                'src/common/backend_registry.cpp']
  if import('fs').is_file('modules/datalake_fdw' / path)
    datalake_sources += files('modules/datalake_fdw' / path)
  else
    datalake_sources += files(datalake_root / path)
  endif
endforeach
shared_module('datalake_fdw', datalake_sources,
  c_args: mod_c_args + ['-fvisibility=hidden'],
  cpp_args: ['-fvisibility=hidden', '-fvisibility-inlines-hidden'],
  link_args: mod_link_args,
  include_directories: [include_directories('modules/datalake_fdw/src',
                                            datalake_root / 'src'),
                        pg_inc, cb_inc],
  name_prefix: '',
  install: true,
  install_dir: pg_pkglibdir,
)
install_data(datalake_root / 'datalake_fdw.control',
             datalake_root / 'datalake_fdw--1.0.sql',
             install_dir: pg_extensiondir)
built_modules += 'datalake_fdw'

###############################################################################
# gp_stats_collector
#
# Cloudberry's gpcontrib/gp_stats_collector: a query's metrics sent to an
# agent's Unix socket as protobuf messages, C++'s protobuf as Cloudberry's
# Makefile builds it, the messages made by protoc (gen/protos).  Its files
# are compiled where they lie, but for the port's copies of the ones
# PostgreSQL 19 or the port changed, under modules/gp_stats_collector/src at
# the same paths, which come first on the include path, as PAX's copies do;
# each says at its head what changed.  gpsc_frozen.c is the port's own, the
# frozen insert of Cloudberry's heapam.c its log table is written with.
# Preloaded, after gp_core, on every node; its extension's script is the
# port's copy of Cloudberry's 1.1, the version its control file names, and
# its control file Cloudberry's, which requires gp_core.
###############################################################################

add_languages('cpp', native: false, required: true)
protobuf_dep = dependency('protobuf', required: true)
protoc = find_program('protoc', required: true)
subdir('modules/gp_stats_collector/gen/protos')
gpsc_root = '../gpcontrib/gp_stats_collector'
gpsc_sources = [gpsc_proto_sources, files('modules/gp_stats_collector/gpsc_frozen.c')]
foreach path : ['src/gp_stats_collector.c',
                'src/hook_wrappers.cpp',
                'src/EventSender.cpp',
                'src/Config.cpp',
                'src/GpscStat.cpp',
                'src/PgUtils.cpp',
                'src/ProcStats.cpp',
                'src/ProtoUtils.cpp',
                'src/UDSConnector.cpp',
                'src/log/LogOps.cpp',
                'src/log/LogSchema.cpp',
                'src/memory/gpdbwrappers.cpp',
                'src/stat_statements_parser/pg_stat_statements_parser.c']
  if import('fs').is_file('modules/gp_stats_collector' / path)
    gpsc_sources += files('modules/gp_stats_collector' / path)
  else
    gpsc_sources += files(gpsc_root / path)
  endif
endforeach
shared_module('gp_stats_collector', gpsc_sources,
  # Cloudberry's C, whose declarations follow statements, as diskquota's do
  c_args: mod_c_args + ['-Wno-declaration-after-statement'],
  # Cloudberry's Makefile's C++ flags, less -Werror, and its symbols its
  # own, as PAX's are
  cpp_args: ['-DGPBUILD', '-Wno-unused-but-set-variable',
             '-fvisibility=hidden', '-fvisibility-inlines-hidden'],
  link_args: mod_link_args,
  dependencies: [protobuf_dep],
  include_directories: [include_directories('modules/gp_stats_collector',
                                            'modules/gp_stats_collector/src',
                                            gpsc_root / 'src',
                                            gpsc_root / 'src' / 'log',
                                            gpsc_root / 'src' / 'stat_statements_parser',
                                            'modules/gp_stats_collector/gen',
                                            'modules/gp_stats_collector/gen/protos'),
                        pg_inc, cb_inc],
  name_prefix: '',
  install: true,
  install_dir: pg_pkglibdir,
)
install_data('modules/gp_stats_collector/gp_stats_collector.control',
             'modules/gp_stats_collector/gp_stats_collector--1.1.sql',
             install_dir: pg_extensiondir)
built_modules += 'gp_stats_collector'

###############################################################################
# pxf_fdw
#
# Cloudberry's gpcontrib/pxf_fdw, the foreign-data wrapper of a PXF server,
# which it asks over HTTP through libcurl: the port's copies of its files
# (modules/pxf_fdw, each headed with what changed) -- the scan and the
# writer, which read and write through gp_exttable's (include/access/
# external.h), the options, and PostgreSQL 19's API drift -- and the one that
# needs no change, compiled where it lies.  They find Cloudberry's headers,
# kept in gpcontrib/pxf_fdw but for pxf_bridge.h, gp_exttable's public ones,
# and GpIdentity in the include overlay compat/gpcontrib/.  The extension's
# script and control file are Cloudberry's as they are.
###############################################################################

if curl_dep.found()
  pxf_root = '../gpcontrib/pxf_fdw'
  shared_module('pxf_fdw',
    files('modules/pxf_fdw/pxf_fdw.c',
          'modules/pxf_fdw/pxf_bridge.c',
          'modules/pxf_fdw/pxf_fragment.c',
          'modules/pxf_fdw/pxf_header.c',
          'modules/pxf_fdw/pxf_option.c',
          'modules/pxf_fdw/libchurl.c',
          'modules/pxf_fdw/pxf_filter.c',
          pxf_root / 'pxf_deparse.c'),
    # Cloudberry's sources, whose declarations follow statements throughout.
    c_args: mod_c_args + ['-Wno-declaration-after-statement'],
    link_args: mod_link_args,
    include_directories: [pg_inc, cb_inc,
                          include_directories('modules/pxf_fdw', 'compat/gpcontrib',
                                              'modules/gp_exttable/include',
                                              pxf_root)],
    dependencies: [curl_dep],
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  install_data(pxf_root / 'pxf_fdw.control', pxf_root / 'pxf_fdw--1.0.sql',
               install_dir: pg_extensiondir)
  built_modules += 'pxf_fdw'
endif

###############################################################################
# gpcloud
#
# Cloudberry's gpcontrib/gpcloud, the s3:// protocol of external tables, whose
# read and write functions gp_exttable calls as Cloudberry's server calls a
# protocol's (include/access/extprotocol.h): its C++ compiled where it lies,
# but for gpcloud.cpp, the port's copy (modules/gpcloud), with what its other
# files ask of Cloudberry's cdbvars.h in the include overlay compat/gpcontrib/.
# Installed as gpcloud.so, with the link gps3ext.so Cloudberry's Makefile
# makes, the library's older name, which its tests still use.  gpcheckcloud,
# its tool, is the same C++ built on its own (S3_STANDALONE); and its unit
# tests, where googletest is in the tree, the test-only program gpcloud_test.
###############################################################################

if get_option('gpcloud')
  gpcloud_root = '../gpcontrib/gpcloud'
  xml_dep = dependency('libxml-2.0', required: true)
  gpcloud_ssl_dep = dependency('openssl', required: true)
  gpcloud_curl_dep = dependency('libcurl', required: true)
  gpcloud_common = []
  foreach f : ['gpreader', 'gpwriter', 's3conf', 's3utils', 's3log', 's3url',
               's3http_headers', 's3interface', 's3restful_service',
               's3bucket_reader', 's3common_reader', 's3common_writer',
               'decompress_reader', 'compress_writer', 's3key_reader',
               's3key_writer']
    gpcloud_common += files(gpcloud_root / 'src' / f + '.cpp')
  endforeach
  gpcloud_lib = files(gpcloud_root / 'lib' / 'http_parser.cpp',
                      gpcloud_root / 'lib' / 'ini.cpp')
  gpcloud_deps = [gpcloud_curl_dep, xml_dep, gpcloud_ssl_dep, zlib_dep,
                  dependency('threads')]
  # OpenSSL 3 deprecates the digests and HMAC gpcloud signs its requests with,
  # which it still provides; and gpcloud is Cloudberry's C++ as it is.
  gpcloud_cpp_args = cpp.get_supported_arguments('-Wno-deprecated-declarations',
                                                 '-Wno-unused-result',
                                                 '-Wno-missing-field-initializers')

  shared_module('gpcloud',
    files('modules/gpcloud/gpcloud.cpp') + gpcloud_common + gpcloud_lib,
    cpp_args: gpcloud_cpp_args,
    link_args: mod_link_args,
    include_directories: [include_directories('compat/gpcontrib'), pg_inc, cb_inc,
                          include_directories('modules/gp_exttable/include',
                                              gpcloud_root / 'include',
                                              gpcloud_root / 'lib')],
    dependencies: gpcloud_deps,
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  meson.add_install_script('sh', '-c',
    'ln -sf gpcloud.so "${DESTDIR}@0@/gps3ext.so"'.format(pg_pkglibdir))
  built_modules += 'gpcloud'

  executable('gpcheckcloud',
    files(gpcloud_root / 'bin' / 'gpcheckcloud' / 'gpcheckcloud.cpp') +
      gpcloud_common + gpcloud_lib,
    cpp_args: gpcloud_cpp_args + ['-DS3_STANDALONE', '-DS3_STANDALONE_CHECKCLOUD'],
    include_directories: include_directories(gpcloud_root / 'include',
                                              gpcloud_root / 'lib'),
    dependencies: gpcloud_deps,
    install: true,
    install_dir: pg['bindir'],
  )
  built_modules += 'gpcheckcloud (program)'
endif

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
# gpfts
#
# Cloudberry's external FTS, a program of its own, for what gp_core's FTS
# cannot do: fail over from the coordinator it runs on.  gpfts watches the
# coordinator and promotes its standby, several instances electing a leader
# through an etcd lease (bin/gpfts).  Cloudberry's etcd client -- an HTTP
# client of etcd's JSON gateway, the keys, and its programs' log -- is
# compiled where it lies, unchanged, through an include overlay
# (compat/gpfts/), as a frontend's, with the frontend's port and common
# libraries; it needs libcurl and jansson.  The configuration file gpfts
# reads by default, and Cloudberry's scripts that bring a failed coordinator
# or standby back as the standby, are installed where Cloudberry's are, in
# bin/config and bin/failover; its scripts for segments are not, the
# segments being gp_core's FTS's.
###############################################################################

gpfts_curl_dep = dependency('libcurl', required: get_option('gpfts'))
jansson_dep = dependency('jansson', required: get_option('gpfts'))
if gpfts_curl_dep.found() and jansson_dep.found()
  executable('gpfts',
    files('bin/gpfts/fts.c',
          'bin/gpfts/ftsprobe.c',
          'bin/gpfts/fts_etcd.c',
          '../src/backend/utils/etcd_lib/etcd.c',
          '../src/common/etcdutils.c',
          '../src/fe_utils/log.c'),
    c_args: ['-DFRONTEND'] + cc.get_supported_arguments('-Wno-pointer-sign'),
    include_directories: [include_directories('bin/gpfts', 'compat/gpfts'),
                          pg_inc, pg_client_inc],
    dependencies: [libpq_dep, gpfts_curl_dep, jansson_dep],
    link_args: [pg_libdir / 'libpgcommon.a', pg_libdir / 'libpgport.a', '-lm'],
    # The server's libpq through the run path, as the modules find it, and
    # not only where LD_LIBRARY_PATH names the server's library directory.
    build_rpath: pg_libdir,
    install_rpath: pg_libdir,
    install: true,
    install_dir: pg['bindir'],
  )
  install_data('../src/bin/gpfts/config/cbdb_etcd_default.conf',
               install_dir: pg['bindir'] / 'config')
  install_data('../src/bin/gpfts/failover/master_check_back.sh',
               '../src/bin/gpfts/failover/standby_check_back.sh',
               install_dir: pg['bindir'] / 'failover',
               install_mode: 'rwxr-xr-x')
  built_modules += 'gpfts (program)'
endif

###############################################################################
# gpMgmt
#
# Cloudberry's management tools -- gpinitsystem, gpstart, gpstop, gpstate,
# gpconfig, gprecoverseg and the rest -- installed beside the server, which
# they drive through PostgreSQL 19's own tools.  See gpMgmt/meson.build.
###############################################################################

if get_option('gpmgmt')
  subdir('gpMgmt')
  built_modules += 'gpMgmt (tools)'
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

  # The functions of Cloudberry's regress.so that its tests load and the
  # port serves: the greenplum suite links it into the directory the tests
  # load regress.so from.
  shared_module('cb_regress', files('test/greenplum/cb_regress.c'),
    c_args: mod_c_args,
    link_args: mod_link_args,
    include_directories: [pg_inc, cb_inc],
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  built_modules += 'cb_regress (test only)'

  # An OAuth validator and a client that logs in with a token, for the
  # security suite's OAuth logins: PostgreSQL 19's own, of its
  # oauth_validator test module, are not installed with the server.
  shared_module('gp_oauth_probe', files('test/security/gp_oauth_probe.c'),
    c_args: mod_c_args,
    link_args: mod_link_args,
    include_directories: [pg_inc, cb_inc],
    name_prefix: '',
    install: true,
    install_dir: pg_pkglibdir,
  )
  executable('gp_oauth_client', files('test/security/gp_oauth_client.c'),
    include_directories: [pg_client_inc],
    dependencies: [libpq_dep],
    build_rpath: pg_libdir,
    install_rpath: pg_libdir,
    install: true,
    install_dir: pg['bindir'],
  )
  built_modules += 'gp_oauth_probe and gp_oauth_client (test only)'

  # The client Cloudberry's resource_queue test runs, a libpq program of its
  # suite's, compiled where it lies, unmodified (it declares a variable it
  # does not use): the greenplum suite runs it from where it is installed.
  executable('extended_protocol_resqueue',
    files('../src/test/regress/extended_protocol_resqueue.c'),
    c_args: ['-Wno-unused-variable'],
    include_directories: [pg_client_inc],
    dependencies: [libpq_dep],
    build_rpath: pg_libdir,
    install_rpath: pg_libdir,
    install: true,
    install_dir: pg['bindir'],
  )
  built_modules += 'extended_protocol_resqueue (test only)'

  # distributed_transactions' client, which sets debug_dtm_action by
  # Cloudberry's names: compiled where it lies, unmodified, its PQexec()
  # one that respells them (test/greenplum/twophase_respell.c).
  executable('twophase_pqexecparams',
    files('../src/test/regress/twophase_pqexecparams.c',
          'test/greenplum/twophase_respell.c'),
    c_args: ['-DPQexec=cb_respell_PQexec'],
    include_directories: [pg_client_inc],
    dependencies: [libpq_dep],
    build_rpath: pg_libdir,
    install_rpath: pg_libdir,
    install: true,
    install_dir: pg['bindir'],
  )
  built_modules += 'twophase_pqexecparams (test only)'

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

  # gpcloud's unit tests, Cloudberry's googletest program, built as its test
  # Makefile builds it -- each test file includes the source it tests, with
  # S3_STANDALONE -- where the tree has googletest: a git submodule of
  # Cloudberry's, which the image clones at the commit the tree names, into
  # PAX's copy of it (Dockerfile.cbext), and which gpcloud's tests are built
  # against too.
  gtest_root = '../contrib/pax_storage/src/cpp/contrib/googletest'
  if get_option('gpcloud') and fs.is_file(gtest_root / 'googletest' / 'src' / 'gtest-all.cc')
    gpcloud_tests = []
    foreach f : ['gpreader', 'gpwriter', 's3conf', 's3utils', 's3log', 's3url',
                 's3http_headers', 's3interface', 's3restful_service',
                 's3bucket_reader', 's3common_reader', 's3common_writer',
                 'decompress_reader', 'compress_writer', 's3key_reader',
                 's3key_writer']
      gpcloud_tests += files(gpcloud_root / 'test' / f + '_test.cpp')
    endforeach
    executable('gpcloud_test',
      gpcloud_tests + files(gtest_root / 'googletest' / 'src' / 'gtest-all.cc',
                            gtest_root / 'googlemock' / 'src' / 'gmock-all.cc',
                            gtest_root / 'googletest' / 'src' / 'gtest_main.cc'),
      cpp_args: gpcloud_cpp_args + ['-DS3_STANDALONE'] +
        cpp.get_supported_arguments('-Wno-sign-compare', '-Wno-unused-variable'),
      include_directories: include_directories(gpcloud_root / 'src',
                                                gpcloud_root / 'include',
                                                gpcloud_root / 'lib',
                                                gtest_root / 'googletest' / 'include',
                                                gtest_root / 'googletest',
                                                gtest_root / 'googlemock' / 'include',
                                                gtest_root / 'googlemock'),
      dependencies: gpcloud_deps,
      install: true,
      install_dir: pg['bindir'],
    )
    built_modules += 'gpcloud_test (test only)'
  endif

  # PAX's unit tests, Cloudberry's googletest programs, in a module a
  # backend runs them in (pax/meson.build, pax/src/pax_gtest.cc): PAX
  # compiled in again, as its own test program had it, and without
  # optimization, as Cloudberry builds them -- a test replaces a function of
  # PAX's or of the server's by patching its code (cpp-stub), which a call
  # the compiler inlined would pass by.
  if pax_gtest_sources.length() > 0
    shared_module('pax_gtest',
      files('modules/pax/pax_catalog.c', 'modules/pax/pax_cbcore.c',
            'modules/pax/pax_numeric.c') + pax_sources + pax_gtest_sources,
      c_args: mod_c_args,
      cpp_args: pax_cpp_args + ['-DRUN_GTEST'],
      link_args: mod_link_args,
      dependencies: pax_deps + [dependency('threads')],
      include_directories: [pg_inc, cb_inc, pax_inc, pax_gtest_inc],
      override_options: ['optimization=0'],
      build_rpath: pg_libdir,
      install_rpath: pg_libdir,
      name_prefix: '',
      install: true,
      install_dir: pg_pkglibdir,
    )
    built_modules += 'pax_gtest (test only)'
  endif

  # Cloudberry's extension for diskquota's tests (control/test), whose
  # functions are diskquota's library's.
  install_data(
    'test/diskquota/diskquota_test.control',
    'test/diskquota/diskquota_test--1.0.sql',
    install_dir: pg_extensiondir,
  )
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
