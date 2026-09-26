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
Certificates between nodes (decision 5), for the tools that change a
cluster after gpinitsystem has made it.  The port's own (pg19/gpMgmt).

A node of a cluster gpinitsystem made with NODE_SSL_DIR shows its
certificate to the node it connects to, gp_core's connections by the
node's settings gp.internal_sslmode, sslcert, sslkey, sslrootcert and
sslcrl, and takes another node's for any role: pg_hba.conf's lines for
the nodes' addresses are "hostssl ... cert map=gpnodes", and pg_ident.conf
maps the nodes' common name to every role (SET_PORT_SETTINGS and
SECURE_PG_HBA, lib/gp_bash_functions.sh).  What a tool adds afterwards
follows the node it is added to: a line that trusts a node's address
becomes such a line (secure_hba_entry()), and a node that streams from
another does so over TLS with the same certificate (replication_options()).
"""

import os
import re

from gppylib import pgconf

# pg_ident.conf's map of the nodes' common name to every role.
MAP_NAME = 'gpnodes'

# gp_core's settings of the TLS between nodes, and libpq's options they are.
SETTINGS = (('gp.internal_sslmode', 'sslmode'),
            ('gp.internal_sslcert', 'sslcert'),
            ('gp.internal_sslkey', 'sslkey'),
            ('gp.internal_sslrootcert', 'sslrootcert'),
            ('gp.internal_sslcrl', 'sslcrl'))

_TRUST = re.compile(r'^(\s*)host(\s+\S+\s+\S+\s+\S+\s+)trust\s*$')


def replication_options(datadir):
    """
    libpq's options of the node's TLS to other nodes, as its configuration
    files give them, each set one: {} for a node without certificates.
    """
    path = os.path.join(datadir, 'postgresql.conf')
    if not os.path.exists(path):
        return {}
    conf = pgconf.readfile(path)
    options = {}
    for setting, option in SETTINGS:
        value = conf.str(setting)
        if value:
            options[option] = value
    return options


def uses_certificates(datadir):
    """Whether the node shows its certificate to other nodes."""
    return 'sslcert' in replication_options(datadir)


def secure_hba_entry(entry):
    """
    A pg_hba.conf line that trusts a node's address, "host <database>
    <user> <address> trust", as the node's certificate over TLS; any other
    line as it is.
    """
    ending = '\n' if entry.endswith('\n') else ''
    match = _TRUST.match(entry.rstrip('\n'))
    if match is None:
        return entry
    return '%shostssl%scert map=%s%s' % (match.group(1), match.group(2), MAP_NAME, ending)
