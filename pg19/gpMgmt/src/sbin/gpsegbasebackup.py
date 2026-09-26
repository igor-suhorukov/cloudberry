#!/usr/bin/env python3
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
"""
gpsegbasebackup.py: Cloudberry's pg_basebackup, as the tools call it, on
PostgreSQL 19's.  The port's own (pg19/gpMgmt): gppylib's PgBaseBackup runs
it where Cloudberry's runs its patched pg_basebackup.

It takes what the tools give Cloudberry's -- the options PostgreSQL 19's
has, and --target-gp-dbid, --force-overwrite and -E -- and does what those
three do around PostgreSQL 19's pg_basebackup (gppylib/nodecopy.py): an
emptied target, the source's tablespaces mapped to the new node's
directories, the paths -E names removed from the copy, and the new node's
dbid, and its WAL receiver's name where -R is given, written into it.
"""

import argparse
import os
import sys

try:
    from gppylib import nodecopy
except ImportError as e:
    sys.exit('Cannot import modules.  Please check that you have sourced cloudberry-env.sh.  Detail: ' + str(e))


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('-D', '--pgdata', dest='target', required=True)
    parser.add_argument('-h', '--host', dest='host', required=True)
    parser.add_argument('-p', '--port', dest='port', required=True)
    parser.add_argument('-U', '--username', dest='user')
    parser.add_argument('-c', '--checkpoint', dest='checkpoint')
    parser.add_argument('-C', '--create-slot', dest='create_slot', action='store_true')
    parser.add_argument('-S', '--slot', dest='slot')
    parser.add_argument('-X', '--wal-method', dest='wal_method')
    parser.add_argument('-R', '--write-recovery-conf', dest='recovery', action='store_true')
    parser.add_argument('--no-verify-checksums', dest='no_verify', action='store_true')
    parser.add_argument('-P', '--progress', dest='progress', action='store_true')
    parser.add_argument('-v', '--verbose', dest='verbose', action='store_true')
    # Cloudberry's own
    parser.add_argument('--target-gp-dbid', dest='dbid', type=int, required=True)
    parser.add_argument('--force-overwrite', dest='force', action='store_true')
    parser.add_argument('-E', '--exclude', dest='exclude', action='append', default=[])
    args = parser.parse_args()

    target = os.path.abspath(args.target)
    if args.force:
        nodecopy.empty_directory(target)

    source_dbid, locations = nodecopy.source_tablespaces(args.host, args.port, args.user)
    maps = []
    for loc in locations:
        nodecopy.empty_directory(os.path.join(loc, str(args.dbid)))
        if os.path.isdir(os.path.join(loc, str(args.dbid))):
            os.rmdir(os.path.join(loc, str(args.dbid)))
        maps += ['-T', '%s/%d=%s/%d' % (loc, source_dbid, loc, args.dbid)]

    argv = ['pg_basebackup', '-D', target, '-h', args.host, '-p', str(args.port)]
    if args.user:
        argv += ['-U', args.user]
    if args.checkpoint:
        argv += ['-c', args.checkpoint]
    if args.create_slot:
        argv += ['--create-slot']
    if args.slot:
        argv += ['--slot', args.slot]
    if args.wal_method:
        argv += ['--wal-method', args.wal_method]
    if args.recovery:
        argv += ['--write-recovery-conf']
    if args.no_verify:
        argv += ['--no-verify-checksums']
    if args.progress:
        argv += ['--progress']
    if args.verbose:
        argv += ['--verbose']
    argv += maps

    sys.stdout.flush()
    rc = nodecopy.run(argv)
    if rc != 0:
        return rc

    for path in args.exclude:
        full = os.path.normpath(os.path.join(target, path))
        if full.startswith(target + os.sep) and os.path.lexists(full):
            if os.path.isdir(full) and not os.path.islink(full):
                nodecopy.shutil.rmtree(full)
            else:
                os.remove(full)
    nodecopy.clear_log_directory(target)
    nodecopy.settle(target, args.dbid, recovery=args.recovery, slot=args.slot,
                    source_host=args.host, source_port=args.port)
    return 0


if __name__ == '__main__':
    sys.exit(main())
