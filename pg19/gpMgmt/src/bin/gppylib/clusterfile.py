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
The cluster file, which the port keeps where Cloudberry keeps its catalog
gp_segment_configuration.  The port's own (pg19/gpMgmt).

An extension can make no shared catalog, and a node has to know the cluster
before any database is open, so gp_core reads the nodes from a file, the one
the setting gp.cluster_config names, as each node starts
(pg19/modules/gp_core/gp_cluster.c): a line per node, "dbid content role host
port datadir", the role the one the node prefers, "p" or "m", and the data
directory the rest of the line.  Which node of a content is its primary now,
and whether it is up and in sync, is FTS's, which the coordinator writes to
gpsegconfig_dump in its data directory, a line per node, "dbid content role
preferred_role mode status port host address".  gp_segment_configuration is a
view over the two, and what changes the cluster -- gp_add_segment_mirror()
and the rest -- rewrites the coordinator's file.

What the tools need of them without a server, and what a server cannot do for
them:

  - read the cluster while its coordinator is down, as gpstart does before it
    starts the segments, where Cloudberry's started the coordinator in utility
    mode to read its catalog (read_segments());
  - give every node the coordinator's file, which a node reads only as it
    starts: a node the tools make or move has to find itself in its own copy,
    on its own host (distribute()).
"""

import base64
import os
import socket

from gppylib import pgconf

CLUSTER_CONFIG_SETTING = 'gp.cluster_config'
DUMP_FILE = 'gpsegconfig_dump'


class ClusterFileError(Exception):
    pass


def config_path(datadir):
    """
    The file gp.cluster_config names for the node of datadir, as an absolute
    path -- a relative one is the data directory's, where the server looks
    for it -- or None if the node names none.
    """
    conf = pgconf.readfile(os.path.join(datadir, 'postgresql.conf'))
    value = conf.str(CLUSTER_CONFIG_SETTING)
    if not value:
        return None
    if os.path.isabs(value):
        return value
    return os.path.normpath(os.path.join(datadir, value))


def parse(text, path='cluster file'):
    """
    The nodes a cluster file lists, in its order, each a dict of dbid,
    content, role, host, port and datadir, read as gp_cluster.c reads them.
    """
    nodes = []
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.split('#', 1)[0]
        fields = line.split(None, 5)
        if not fields:
            continue
        if len(fields) < 5:
            raise ClusterFileError('line %d of %s has %d fields, not "dbid content role host port datadir"'
                                   % (lineno, path, len(fields)))
        try:
            dbid, content, port = int(fields[0]), int(fields[1]), int(fields[4])
        except ValueError:
            raise ClusterFileError('line %d of %s: dbid, content id and port are numbers' % (lineno, path))
        if fields[2] not in ('p', 'm'):
            raise ClusterFileError('line %d of %s: the role is "p" or "m", not "%s"' % (lineno, path, fields[2]))
        nodes.append({'dbid': dbid, 'content': content, 'role': fields[2],
                      'host': fields[3], 'port': port,
                      'datadir': fields[5].strip() if len(fields) > 5 else ''})
    return nodes


def read(path):
    with open(path) as f:
        return parse(f.read(), path)


def read_states(coordinator_datadir):
    """
    What FTS last wrote of each node, by dbid: (content, role, mode,
    status).  Nothing before its first write, when each node has the role
    the file gives it, is up, and is not known to be in sync.
    """
    states = {}
    path = os.path.join(coordinator_datadir, DUMP_FILE)
    if not os.path.exists(path):
        return states
    with open(path) as f:
        for line in f:
            fields = line.split()
            if len(fields) < 6:
                continue
            states[int(fields[0])] = (int(fields[1]), fields[2], fields[4], fields[5])
    return states


def read_segments(coordinator_datadir):
    """
    The rows gp_segment_configuration would give, as Segments, read from the
    coordinator's files and not its server: the file's nodes, with the role,
    mode and status FTS last wrote of each, as gp_cluster.c takes them.  A
    server that names no cluster file is a single node, the one row
    gp_segment_configuration gives of it: this host's.
    """
    from gppylib.gparray import Segment

    path = config_path(coordinator_datadir)
    if path is None:
        conf = pgconf.readfile(os.path.join(coordinator_datadir, 'postgresql.conf'))
        host = socket.gethostname()
        return [Segment(content=-1, preferred_role='p', dbid=conf.int('gp.dbid', 1),
                        role='p', mode='n', status='u', hostname=host, address=host,
                        port=conf.int('port', 5432), datadir=coordinator_datadir)]
    states = read_states(coordinator_datadir)
    segments = []
    for node in read(path):
        role, mode, status = node['role'], 'n', 'u'
        state = states.get(node['dbid'])
        if state is not None and state[0] == node['content']:
            role, mode, status = state[1], state[2], state[3]
        segments.append(Segment(content=node['content'], preferred_role=node['role'],
                                dbid=node['dbid'], role=role, mode=mode, status=status,
                                hostname=node['host'], address=node['host'],
                                port=node['port'], datadir=node['datadir']))
    return segments


def write(path, text):
    """The file, whole and at once: a new one, synced, renamed over the old."""
    tmp = '%s.gptmp%d' % (path, os.getpid())
    with open(tmp, 'w') as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, path)


def encode(text):
    return base64.urlsafe_b64encode(text.encode()).decode()


def decode(value):
    return base64.urlsafe_b64decode(value.encode()).decode()


def distribute(gparray, coordinator_datadir, segments=None, pool=None):
    """
    The coordinator's cluster file to the nodes of the array, or to the
    given segments: on each host, gpsegclusterfile.py writes it where each
    node's gp.cluster_config says, as the node will read it when it next
    starts.  A node that names none, and the coordinator itself, are left
    alone.
    """
    from gppylib.commands.base import Command, REMOTE, WorkerPool

    path = config_path(coordinator_datadir)
    if path is None:
        raise ClusterFileError('the coordinator in %s names no cluster file (%s)'
                               % (coordinator_datadir, CLUSTER_CONFIG_SETTING))
    with open(path) as f:
        content = encode(f.read())

    if segments is None:
        segments = gparray.getDbList()
    byhost = {}
    for seg in segments:
        if seg.getSegmentContentId() == -1 and seg.getSegmentDataDirectory() == coordinator_datadir:
            continue
        byhost.setdefault(seg.getSegmentHostName(), []).append(seg.getSegmentDataDirectory())
    if not byhost:
        return

    own_pool = pool is None
    if own_pool:
        pool = WorkerPool(numWorkers=min(len(byhost), 16))
    try:
        for host, datadirs in byhost.items():
            cmd = Command('give the cluster file to the nodes on %s' % host,
                          '$GPHOME/sbin/gpsegclusterfile.py --content %s %s'
                          % (content, ' '.join("-D '%s'" % d for d in datadirs)),
                          ctxt=REMOTE, remoteHost=host)
            pool.addCommand(cmd)
        pool.join()
        failed = [c for c in pool.getCompletedItems() if not c.was_successful()]
        pool.empty_completed_items()
        if failed:
            raise ClusterFileError('the cluster file could not be given to the nodes on %s: %s'
                                   % (', '.join(c.remoteHost for c in failed),
                                      '; '.join(c.get_results().stderr.strip() for c in failed)))
    finally:
        if own_pool:
            pool.haltWork()
            pool.joinWorkers()
