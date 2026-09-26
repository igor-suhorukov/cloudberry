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
A node made from another's files, by PostgreSQL 19's pg_basebackup or
pg_rewind, as Cloudberry's patched ones make it.  The port's own
(pg19/gpMgmt), run on the host of the node made, by sbin/gpsegbasebackup.py
and sbin/gpsegrewind.py; and the node started, waited for by
sbin/gpsegrecovery.py until it streams.

What Cloudberry's tools do that PostgreSQL 19's do not:

  - pg_basebackup --target-gp-dbid N lays each tablespace out as the new
    node's, <location>/<N> where the source's is <location>/<its dbid>, and
    writes the new node's dbid, gp_dbid, into its internal.auto.conf;
  - pg_basebackup --force-overwrite writes into a directory that is not
    empty, and -E leaves a path of the source's out;
  - pg_rewind --slot writes the slot the node streams from, and neither
    pg_rewind copies internal.auto.conf, so the node keeps its dbid;
  - -R of either writes the node's WAL receiver's name, gp_walreceiver,
    which a primary, and FTS, know a mirror by;
  - Cloudberry's server leaves a node's log/ out of a base backup.

On the port the dbid is gp.dbid, which the copy takes from the source's
postgresql.conf and postgresql.auto.conf: it is written into the new node's
postgresql.auto.conf, which the server reads last.
"""

import os
import re
import shutil
import subprocess
import time

from gppylib import nodetls, pgconf

RECEIVER_NAME = 'gp_walreceiver'
AUTO_CONF = 'postgresql.auto.conf'


def node_dbid(datadir):
    """The dbid a node's configuration files give it, or None."""
    return pgconf.readfile(os.path.join(datadir, 'postgresql.conf')).int('gp.dbid')


def source_tablespaces(host, port, user=None):
    """
    The source's own dbid, and the location of each of its tablespaces, as
    its pg_tablespace_location() says them: the one CREATE TABLESPACE was
    given (gp_core, through O32), under which each node's directory is the one
    of its dbid.
    """
    import pgdb
    conninfo = {'host': host, 'port': int(port), 'database': 'template1'}
    if user:
        conninfo['user'] = user
    conn = pgdb.connect(**conninfo)
    try:
        cur = conn.cursor()
        cur.execute("SELECT pg_catalog.current_setting('gp.dbid')")
        dbid = int(cur.fetchone()[0])
        cur.execute("SELECT pg_catalog.pg_tablespace_location(oid) FROM pg_catalog.pg_tablespace "
                    "WHERE spcname NOT IN ('pg_default', 'pg_global')")
        locations = [row[0] for row in cur.fetchall() if row[0]]
    finally:
        conn.close()
    return dbid, locations


def empty_directory(path):
    """What --force-overwrite lets Cloudberry's pg_basebackup write into: a
    directory PostgreSQL's will only take empty."""
    if os.path.isdir(path):
        for name in os.listdir(path):
            full = os.path.join(path, name)
            if os.path.isdir(full) and not os.path.islink(full):
                shutil.rmtree(full)
            else:
                os.remove(full)


def _replace_settings(path, settings):
    """
    The settings given, each once, at the end of a configuration file, every
    earlier line of the same name gone: postgresql.auto.conf, which ALTER
    SYSTEM and pg_basebackup -R write, and the server reads last.
    """
    lines = []
    if os.path.exists(path):
        with open(path) as f:
            lines = f.readlines()
    names = set(settings)
    kept = [line for line in lines
            if line.split('=', 1)[0].strip() not in names]
    if kept and not kept[-1].endswith('\n'):
        kept[-1] += '\n'
    for name, value in settings.items():
        kept.append('%s = %s\n' % (name, value))
    tmp = path + '.gptmp'
    with open(tmp, 'w') as f:
        f.writelines(kept)
    os.rename(tmp, path)


def _quote(value):
    return "'" + value.replace("'", "''") + "'"


def conninfo_values(conninfo):
    """A libpq connection string's keywords and values, unquoted."""
    values = {}
    for key, value in re.findall(r"(\w+)\s*=\s*('(?:[^'\\]|\\.)*'|\S+)", conninfo or ''):
        if value.startswith("'"):
            value = re.sub(r"\\(.)", r"\1", value[1:-1])
        values[key] = value
    return values


def primary_conninfo(datadir):
    """The primary_conninfo the node's postgresql.auto.conf ends with, or None."""
    value = None
    path = os.path.join(datadir, AUTO_CONF)
    if os.path.exists(path):
        conf = pgconf.readfile(path)
        value = conf.str('primary_conninfo')
    return value


def settle(datadir, dbid, recovery=False, slot=None, source_host=None, source_port=None):
    """
    What Cloudberry's pg_basebackup and pg_rewind leave a copy with that
    PostgreSQL 19's do not: its own dbid and, as a node that streams from its
    source, the name its WAL receiver streams under and the slot it streams
    from.  Written into its postgresql.auto.conf.  Where the nodes
    authenticate each other by certificates, the node streams over TLS with
    its certificate, as gp_core's connections go (nodetls.py).
    """
    settings = {'gp.dbid': str(dbid)}
    if recovery:
        conninfo = primary_conninfo(datadir)
        if conninfo is None:
            conninfo = 'host=%s port=%s' % (source_host, source_port)
        tls = nodetls.replication_options(datadir)
        for option in ['application_name'] + list(tls):
            conninfo = re.sub(r"\s*\b%s=('(?:[^'\\]|\\.)*'|\S*)" % option, '', conninfo).strip()
        for option, value in tls.items():
            conninfo += " %s='%s'" % (option, value.replace('\\', '\\\\').replace("'", "\\'"))
        settings['primary_conninfo'] = _quote('%s application_name=%s' % (conninfo, RECEIVER_NAME))
        if slot:
            settings['primary_slot_name'] = _quote(slot)
    _replace_settings(os.path.join(datadir, AUTO_CONF), settings)


def clear_log_directory(datadir):
    """The source's logs, which Cloudberry's server leaves out of a base backup."""
    logdir = os.path.join(datadir, 'log')
    if os.path.isdir(logdir):
        for name in os.listdir(logdir):
            full = os.path.join(logdir, name)
            if os.path.isfile(full):
                os.remove(full)


def local_host(datadir):
    """
    Where a node of this host takes connections, as its postmaster.pid says:
    its first socket directory, or else its first listen address.
    """
    try:
        with open(os.path.join(datadir, 'postmaster.pid')) as f:
            lines = f.read().split('\n')
    except OSError:
        return 'localhost'
    if len(lines) > 4 and lines[4].strip():
        return lines[4].strip()
    if len(lines) > 5 and lines[5].strip() not in ('', '*'):
        return lines[5].strip()
    return 'localhost'


def wait_streaming(datadir, port, timeout=60):
    """
    Whether a node of this host just started as a mirror streams from its
    primary within the timeout, as its WAL receiver says: FTS marks a mirror
    up only once its primary has a WAL sender for it.  A hot standby takes
    the question.
    """
    from contextlib import closing
    from gppylib.db import dbconn
    dburl = dbconn.DbURL(hostname=local_host(datadir), port=int(port), dbname='template1')
    deadline = time.time() + timeout
    while True:
        try:
            with closing(dbconn.connect(dburl, logConn=False)) as conn:
                status = dbconn.querySingleton(conn, "SELECT status FROM pg_catalog.pg_stat_wal_receiver")
            if status == 'streaming':
                return True
        except Exception:
            pass
        if time.time() >= deadline:
            return False
        time.sleep(0.2)


def run(argv, stdout=None):
    """Run a PostgreSQL tool, its output to stdout; its return code."""
    return subprocess.call(argv, stdout=stdout, stderr=subprocess.STDOUT)
