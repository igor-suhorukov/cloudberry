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
A leaf partition's HyperLogLog counters, as gpsd and minirepro write them
with the leaf's statistics when asked for (--hll).

Cloudberry keeps a leaf's counter of a column in the last slot of its
pg_statistic row, under kinds 98 and 99, and the tools write it with the row
(gppylib/utils.py, formatInsertValuesList()).  The port keeps it in
gp_internal.leaf_hll, which goes with the pg_statistic row by that row's
xmin (gp_core's gp_partmerge.c): so the counter is read with the row, and
written by a statement after the one that writes the row, which finds the
xmin the new row was given.  PostgreSQL would not take the two as one, the
row's INSERT in a WITH: there the row's stavalues, of type anyarray, are
not coerced to it.
"""

# The counter of a row of pg_statistic, alias pgs, where it has one that goes
# with it: whether ANALYZE read every row for it, and the counter -- two
# columns for the tools' query of the rows, after pgs.*.
LEAF_HLL_COLUMNS = ('(SELECT h.fullscan FROM gp_internal.leaf_hll h '
                    'WHERE h.starelid = pgs.starelid AND h.staattnum = pgs.staattnum '
                    'AND h.staxmin = pgs.xmin LIMIT 1), '
                    '(SELECT h.counter FROM gp_internal.leaf_hll h '
                    'WHERE h.starelid = pgs.starelid AND h.staattnum = pgs.staattnum '
                    'AND h.staxmin = pgs.xmin LIMIT 1)')


def formatLeafHLL(stmt, starelid, staattnum, stainherit, fullscan, counter):
    """
    The tool's statement that inserts a row of pg_statistic, and after it,
    where the row has a counter, the statement that writes the counter.
    """
    if counter is None:
        return stmt
    return (stmt.rstrip('\n') + '\n' +
            'INSERT INTO gp_internal.leaf_hll\n'
            "SELECT starelid, staattnum, xmin, %s, '\\x%s'::bytea FROM pg_statistic\n"
            'WHERE starelid = %s AND staattnum = %d AND stainherit = %s;\n\n'
            % ('true' if fullscan else 'false', bytes(counter).hex(),
               starelid, staattnum, 'true' if stainherit else 'false'))
