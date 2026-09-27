#!/usr/bin/env python3
#
# Ported to PostgreSQL 19: the port's copy of
# gpMgmt/bin/gpcheckcat_modules/unique_index_violation_check.py
# (pg19/gpMgmt/meson.build).  Each segment's duplicates are found by a query
# of its own rows run there, gp_internal.segment_query(), which sends back
# only the segments that have one: the port's gp_dist_random() would gather
# every row whole, and pg_statistic's rows cannot travel (their anyarray
# values, which nothing reads back).

class UniqueIndexViolationCheck:
    unique_indexes_query = """
        select table_oid, index_name, table_name, array_agg(attname) as column_names
        from pg_attribute, (
            select pg_index.indrelid as table_oid, index_class.relname as index_name, table_class.relname as table_name, unnest(pg_index.indkey) as column_index
            from pg_index, pg_class index_class, pg_class table_class
            where pg_index.indisunique='t'
            and index_class.relnamespace = (select oid from pg_namespace where nspname = 'pg_catalog')
            and index_class.relkind = 'i'
            and index_class.oid = pg_index.indexrelid
            and table_class.oid = pg_index.indrelid
        ) as unique_catalog_index_columns
        where attnum = column_index
        and attrelid = table_oid
        group by table_oid, index_name, table_name;
    """

    def __init__(self):
        # A unique index's duplicates, on each segment by a query of the
        # segment's own rows there, and on the coordinator.
        self.violated_segments_query = """
            select distinct(gp_segment_id) from (
                (select gp_segment_id
                from gp_internal.segment_query(%s) as s(gp_segment_id int4))
                union
                (select gp_segment_id
                from %s
                where (%s) is not null
                group by gp_segment_id, %s
                having count(*) > 1)
            ) as violations
        """
        self.segment_query = """
            select gp_segment_id
            from gp_dist_random('%s')
            where (%s) is not null
            group by gp_segment_id, %s
            having count(*) > 1
        """

    def runCheck(self, db_connection):
        unique_indexes = db_connection.query(self.unique_indexes_query).getresult()
        violations = []

        for (table_oid, index_name, table_name, column_names) in unique_indexes:
            column_names = ",".join(column_names)
            sql = self.get_violated_segments_query(table_name, column_names)
            violated_segments = db_connection.query(sql).getresult()
            if violated_segments:
                violations.append(dict(table_oid=table_oid,
                                       table_name=table_name,
                                       index_name=index_name,
                                       column_names=column_names,
                                       violated_segments=[row[0] for row in violated_segments]))

        return violations

    def get_violated_segments_query(self, table_name, column_names):
        segment_query = self.segment_query % (table_name, column_names, column_names)
        return self.violated_segments_query % (
            "'" + segment_query.replace("'", "''") + "'",
            table_name, column_names, column_names
        )
