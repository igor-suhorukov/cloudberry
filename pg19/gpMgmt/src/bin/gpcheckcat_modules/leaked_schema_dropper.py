#
# Ported to PostgreSQL 19: the port's copy of
# gpMgmt/bin/gpcheckcat_modules/leaked_schema_dropper.py
# (pg19/gpMgmt/meson.build).  Cloudberry names a session's temporary schema
# by its session ID, the same on every node, and drops it as the session
# ends, so one whose session is gone has leaked, and gpcheckcat drops it
# through the coordinator.  PostgreSQL 19 names it by the backend's number,
# each node's own, keeps it for the next backend of that number, which
# empties it as it first needs it, and autovacuum drops a table a gone
# backend left there; and on the port a DROP SCHEMA on the coordinator is
# sent to every segment, where a schema of that name is another backend's.
# So this finds, on each node, the temporary schemas that still hold
# relations of a backend that is gone -- no backend of this database has its
# number there (pg_stat_get_backend_dbid()) -- and reports them; it drops
# nothing.

class LeakedSchemaDropper:

    leaked_schema_query = r"""
        SELECT n.nspname AS schema
        FROM   pg_catalog.pg_namespace n
        WHERE  n.nspname ~ '^pg_(toast_)?temp_[0-9]+$'
          AND  EXISTS (SELECT 1 FROM pg_catalog.pg_class c WHERE c.relnamespace = n.oid)
          AND  pg_catalog.pg_stat_get_backend_dbid(
                   pg_catalog.regexp_replace(n.nspname, '^pg_(toast_)?temp_', '')::int)
               IS DISTINCT FROM (SELECT oid FROM pg_catalog.pg_database
                                 WHERE datname = pg_catalog.current_database())
        ORDER BY 1
    """

    def find_leaked_schemas(self, db_connection):
        leaked_schemas = db_connection.query(self.leaked_schema_query)
        return [row[0] for row in leaked_schemas.getresult() if row[0]]
