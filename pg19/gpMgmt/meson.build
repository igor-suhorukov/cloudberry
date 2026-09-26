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

# gpMgmt, Cloudberry's management tools, installed beside the server they
# manage, as Cloudberry installs them: the tools in bin, what they run on
# each host in sbin, gppylib in lib/python, cloudberry-env.sh at the top --
# which makes the server's prefix GPHOME, the directory every tool reaches
# $GPHOME/bin/pg_ctl and the rest through.
#
# As PAX's, two halves:
#
#   Cloudberry's files  installed as they are, wherever they need no change
#                       to run on PostgreSQL 19's stock tools.
#
#   the port's copies   of the files that had to change, under src/ at the
#                       same paths under gpMgmt/, each saying at its head
#                       what changed -- the calls of the tools with options
#                       only Cloudberry's patched pg_basebackup, pg_rewind,
#                       pg_ctl and postgres have, Cloudberry's settings
#                       gp_role and gp_dbid, and the cluster the port keeps
#                       in a file where Cloudberry keeps a catalog.  A path
#                       only src/ has is the port's own.
#
# files.txt says what is installed and where.  Cloudberry's originals stay
# where they are, so a merge from apache/cloudberry still applies to them; a
# change upstream makes to a file the port has copied is ported by hand.

fs = import('fs')

gpmgmt_root = '..' / '..' / 'gpMgmt'

# GPHOME: the prefix of the server, whose bin/ has pg_ctl.
gphome = fs.parent(pg['bindir'])

# What --version says, as Cloudberry's configure writes it: "<version> build
# <build>", which GpVersion parses.
gpmgmt_version = meson.project_version() + ' build dev'

sed = find_program('sed', required: true)
bash = find_program('bash', required: true)

# The files executable where they are, gpMgmt/ or src/, by their paths under
# either: Cloudberry's Makefiles copy gppylib with cp -p, which keeps a
# file's mode, and gpinitstandby runs one of its files, operations/
# initstandby.py, as a program on the coordinator's host.
gpmgmt_executables = run_command(bash, '-c',
  'for d in "$1" "$2"; do (cd "$d" && find . -type f -perm -u+x); done',
  'bash', meson.current_source_dir() / gpmgmt_root, meson.current_source_dir() / 'src',
  check: true).stdout().split()

foreach line : fs.read('files.txt').strip().split('\n')
  words = line.strip().split()
  if words.length() == 0 or words[0].startswith('#')
    continue
  endif
  if words.length() != 2
    error('gpMgmt/files.txt: "@0@" is not "<directory> <path>"'.format(line))
  endif
  dir = words[0]
  path = words[1]

  if fs.is_file('src' / path)
    source = 'src' / path
  elif fs.is_file(gpmgmt_root / path)
    source = gpmgmt_root / path
  else
    error('gpMgmt/files.txt names @0@, which neither gpMgmt/ nor src/ has'.format(path))
  endif

  mode = 'rw-r--r--'
  if dir.startswith('bin') or dir.startswith('sbin') or ('./' + path) in gpmgmt_executables
    mode = 'rwxr-xr-x'
  endif

  install_data(source, install_dir: gphome / dir, install_mode: mode)
endforeach

# $Revision$ in what was installed: the version, as Cloudberry's putversion
# writes it into each tool as its Makefiles install it.
meson.add_install_script('putversion.sh', gphome, gpmgmt_version)

# gp_bash_version.sh, which gpinitsystem's print_version reads: its version,
# as configure writes it from the .in.
custom_target('gpmgmt-gp_bash_version',
  input: gpmgmt_root / 'bin' / 'lib' / 'gp_bash_version.sh.in',
  output: 'gp_bash_version.sh',
  command: [sed, 's,##.*##,' + gpmgmt_version + ',', '@INPUT@'],
  capture: true,
  install: true,
  install_dir: gphome / 'bin' / 'lib',
  install_mode: 'rwxr-xr-x',
)

# What Cloudberry's "postgres --gp-version" prints, which the tools compare
# across the hosts, and which PostgreSQL 19's postgres cannot print: this
# install's version, in the file the port's gp.py reads instead.
custom_target('gpmgmt-gp_version',
  output: 'gp_version',
  command: ['echo', 'postgres (Apache Cloudberry) ' + gpmgmt_version],
  capture: true,
  install: true,
  install_dir: gphome / 'share' / 'greenplum',
)

# cloudberry-env.sh, which every tool sources on each host: GPHOME, PATH,
# PYTHONPATH and LD_LIBRARY_PATH from where the script itself is.
custom_target('gpmgmt-cloudberry-env',
  output: 'cloudberry-env.sh',
  command: [bash, files(gpmgmt_root / 'bin' / 'generate-cloudberry-env.sh')],
  capture: true,
  install: true,
  install_dir: gphome,
)

# The settings no configuration file may hold, which gpconfig refuses to
# write.  Cloudberry lists them as it installs, from its own sources
# (gpconfig_modules/parse_guc_metadata.py); the port's list is PostgreSQL
# 19's -- the entries of src/backend/utils/misc/guc_parameters.dat flagged
# GUC_DISALLOW_IN_FILE, at REL_19_STABLE_CLOUDBERRY -- and the modules' own,
# those their DefineCustom*Variable() calls so flag, kept in share/ by hand.
install_data('share' / 'gucs_disallowed_in_file.txt',
  install_dir: gphome / 'share' / 'greenplum',
)

# ifaddrs, which gpinitsystem runs on each host for the addresses pg_hba.conf
# admits: a program of its own, compiled where it lies.
executable('ifaddrs', files(gpmgmt_root / 'bin' / 'ifaddrs' / 'main.c'),
  install: true,
  install_dir: gphome / 'libexec',
)
