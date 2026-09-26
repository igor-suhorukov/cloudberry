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
gpsegclusterfile.py --content <base64> -D <datadir> [-D <datadir> ...]

Internal use: the cluster file the coordinator keeps (gppylib/clusterfile.py),
written on this host for each node of the data directories given, where the
node's gp.cluster_config says -- what the node reads as it next starts.  A
node that names no cluster file is passed over.  The port's own
(pg19/gpMgmt), run by the tools on each host as Cloudberry's run
gpsegstart.py.
"""

import os
import sys
from optparse import OptionParser

try:
    from gppylib import clusterfile
except ImportError as e:
    sys.exit('Cannot import modules.  Please check that you have sourced cloudberry-env.sh.  Detail: ' + str(e))


def main():
    parser = OptionParser(usage=__doc__.strip().split('\n')[0])
    parser.add_option('--content', dest='content')
    parser.add_option('-D', dest='datadirs', action='append', default=[])
    (options, args) = parser.parse_args()
    if args or options.content is None or not options.datadirs:
        parser.error('--content and at least one -D are required')

    text = clusterfile.decode(options.content)
    clusterfile.parse(text)            # refuse to write what no node could read
    failed = False
    for datadir in options.datadirs:
        if not os.path.isdir(datadir):
            sys.stderr.write('%s is not there; passed over\n' % datadir)
            continue
        try:
            path = clusterfile.config_path(datadir)
            if path is None:
                sys.stderr.write('%s names no cluster file; passed over\n' % datadir)
                continue
            clusterfile.write(path, text)
        except Exception as e:
            sys.stderr.write('could not write the cluster file of %s: %s\n' % (datadir, e))
            failed = True
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
