/* pg19/modules/gp_sql/gp_debug_numsegments--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_debug_numsegments" to load this file. \quit

/*
 * Cloudberry's gp_debug_numsegments, whose functions say how many segments a
 * table this session creates is spread over: the first so many, which makes
 * a partial table, as a cluster's expansion leaves its tables until each is
 * expanded.  The same functions, under the same names, in the schema the
 * extension is created in, as Cloudberry's; gp_sql carries them
 * (distribution.c), and records the count in the table's "gp" label.
 */

-- Set it: a count from 1 to the size of the cluster, or 'full', 'minimal' or
-- 'random'; answers what it is now.
CREATE FUNCTION gp_debug_set_create_table_default_numsegments(text) RETURNS text
AS 'MODULE_PATHNAME', 'gp_debug_set_create_table_default_numsegments'
LANGUAGE C STRICT;

CREATE FUNCTION gp_debug_set_create_table_default_numsegments(integer) RETURNS text
AS 'MODULE_PATHNAME', 'gp_debug_set_create_table_default_numsegments'
LANGUAGE C STRICT;

-- Set it, and make it what a reset returns to; or, with no argument, return
-- to that, which is 'full' until a reset has said otherwise.
CREATE FUNCTION gp_debug_reset_create_table_default_numsegments(text) RETURNS void
AS 'MODULE_PATHNAME', 'gp_debug_reset_create_table_default_numsegments'
LANGUAGE C STRICT;

CREATE FUNCTION gp_debug_reset_create_table_default_numsegments(integer) RETURNS void
AS 'MODULE_PATHNAME', 'gp_debug_reset_create_table_default_numsegments'
LANGUAGE C STRICT;

CREATE FUNCTION gp_debug_reset_create_table_default_numsegments() RETURNS void
AS 'MODULE_PATHNAME', 'gp_debug_reset_create_table_default_numsegments'
LANGUAGE C STRICT;

-- What it is: FULL, MINIMAL, RANDOM or the count.
CREATE FUNCTION gp_debug_get_create_table_default_numsegments() RETURNS text
AS 'MODULE_PATHNAME', 'gp_debug_get_create_table_default_numsegments'
LANGUAGE C STRICT;
