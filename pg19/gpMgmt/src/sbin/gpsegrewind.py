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
gpsegrewind.py: Cloudberry's pg_rewind, as gprecoverseg calls it, on
PostgreSQL 19's.  The port's own (pg19/gpMgmt): gppylib's PgRewind runs it
where Cloudberry's runs its patched pg_rewind.

PostgreSQL 19's pg_rewind has no --slot, and copies every configuration
file of the source, the dbid in it too, where Cloudberry's keeps the node's
internal.auto.conf.  So the node's dbid is read before the rewind and
written back after it, with the slot and, where -R is given, the name its
WAL receiver streams under (gppylib/nodecopy.py).
"""

import argparse
import os
import sys

try:
    from gppylib import nodecopy
except ImportError as e:
    sys.exit('Cannot import modules.  Please check that you have sourced cloudberry-env.sh.  Detail: ' + str(e))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-D', '--target-pgdata', dest='target', required=True)
    parser.add_argument('--source-server', dest='source', required=True)
    parser.add_argument('-R', '--write-recovery-conf', dest='recovery', action='store_true')
    parser.add_argument('-P', '--progress', dest='progress', action='store_true')
    # Cloudberry's own
    parser.add_argument('--slot', dest='slot')
    args = parser.parse_args()

    target = os.path.abspath(args.target)
    dbid = nodecopy.node_dbid(target)
    if dbid is None:
        sys.stderr.write('%s says no gp.dbid: which node it is cannot be kept\n' % target)
        return 1

    argv = ['pg_rewind', '--target-pgdata', target, '--source-server', args.source]
    if args.recovery:
        argv += ['--write-recovery-conf']
    if args.progress:
        argv += ['--progress']
    sys.stdout.flush()
    rc = nodecopy.run(argv)
    if rc != 0:
        return rc

    source = nodecopy.conninfo_values(args.source)
    nodecopy.settle(target, dbid, recovery=args.recovery, slot=args.slot,
                    source_host=source.get('host'), source_port=source.get('port'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
