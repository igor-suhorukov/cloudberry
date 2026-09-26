#!/usr/bin/env python3
#
# Ported to PostgreSQL 19: the port's copy of
# gpMgmt/bin/gppylib/operations/segment_tablespace_locations.py
# (pg19/gpMgmt/meson.build).  The port has no gp_tablespace_location(), which
# asks every segment where a tablespace is: every node's
# pg_tablespace_location() says the location CREATE TABLESPACE was given
# (gp_core, through O32), and each node's directory is the one of its dbid
# under it, as Cloudberry's is.
#
from contextlib import closing
from gppylib.db import dbconn

# get tablespace locations
def get_tablespace_locations(all_hosts, mirror_data_directory):
    """
    to get user defined tablespace locations for all hosts or a specific mirror data directory.
    :param all_hosts: boolean type to indicate if tablespace locations should be fetched from all hosts.
                      Only gpdeletesystem will call it with True
    :param mirror_data_directory: string type to fetch tablespace locations for a specific mirror data directory.
                      Only gpmovemirrors will call it with specific data directory
    :return: list of tablespace locations
    """
    tablespace_locations = []
    # every node's location, the one CREATE TABLESPACE was given
    oid_subq = """ (SELECT pg_catalog.pg_tablespace_location(oid) AS tblspc_loc
                    FROM pg_catalog.pg_tablespace
                    WHERE spcname NOT IN ('pg_default', 'pg_global')
                    ) AS t """

    with closing(dbconn.connect(dbconn.DbURL())) as conn:
        if all_hosts:
            tablespace_location_sql = """
                SELECT c.hostname, t.tblspc_loc||'/'||c.dbid tblspc_loc
                FROM {oid_subq}
                    CROSS JOIN gp_segment_configuration AS c
                """ .format(oid_subq=oid_subq)
        else:
            tablespace_location_sql = """
                SELECT c.hostname,c.content, t.tblspc_loc||'/'||c.dbid tblspc_loc
                FROM {oid_subq}
                    JOIN gp_segment_configuration AS c
                    ON c.role='m' AND c.datadir ='{mirror_data_directory}'
                """ .format(oid_subq=oid_subq, mirror_data_directory=mirror_data_directory)
        res = dbconn.query(conn, tablespace_location_sql)
        for r in res:
            tablespace_locations.append(r)
    return tablespace_locations
