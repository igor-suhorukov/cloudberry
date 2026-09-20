/* pg19/modules/gp_orca/gp_orca--1.0.sql */

-- complain if the script is sourced by psql rather than CREATE EXTENSION
\echo Use "CREATE EXTENSION gp_orca" to load this file. \quit

CREATE SCHEMA gp_orca;
GRANT USAGE ON SCHEMA gp_orca TO PUBLIC;

/*
 * What ORCA is linked in, and whether it comes up in this backend.
 *
 * "xforms" is the number of transformation rules this ORCA was built with.
 * It is what tells one ORCA from another: Cloudberry's tree and pgorca's
 * carry different rules, so this is how to see which one the server is
 * actually running.
 */
CREATE FUNCTION gp_orca.version(
	OUT source text,
	OUT xforms int,
	OUT initialized boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_version'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.version() IS
	'which ORCA is linked in, and whether it has been brought up here';

/*
 * Every transformation rule this ORCA carries, by name.
 *
 * A rule is what ORCA searches with, so this is what the optimizer can do.
 * Cloudberry's ORCA carries three rules the single-node fork does not:
 * ExfGet2ParallelTableScan, ExfPushPartialAggBelowJoin and
 * ExfImplementHashSequenceProject.
 */
CREATE FUNCTION gp_orca.xforms()
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'gp_orca_xforms'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.xforms() IS
	'the transformation rules this ORCA was built with';

/*
 * What the server-side checks say about a query, before ORCA is asked to plan
 * it.  ORCA declines some shapes on these answers.
 *
 * "orderby_ordering_op" is the KNN shape: ORDER BY over an ordering operator
 * on a plain column, which PostgreSQL's planner turns into a GiST index scan
 * and ORCA cannot, so ORCA leaves the query to the planner.
 *
 * "non_default_collation" is whether anything in the query carries a collation
 * that is not the default one.  Cloudberry's own copy of this check still
 * carries a merge marker from PostgreSQL 9.1 ("GPDB_91_MERGE_FIXME:
 * collation"), so it answers that one question and no more.
 */
CREATE FUNCTION gp_orca.explain_refusal(
	sql text,
	OUT orderby_ordering_op boolean,
	OUT non_default_collation boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_explain_refusal'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gp_orca.explain_refusal(text) FROM PUBLIC;

COMMENT ON FUNCTION gp_orca.explain_refusal(text) IS
	'what the checks in front of ORCA say about a query';

/*
 * The compat layer, seen from SQL.
 *
 * pg19/orca/compat/ re-implements what ORCA asks of a PostgreSQL that
 * Cloudberry had patched -- functions that live in PostgreSQL files
 * Cloudberry modified, which the port does not build.  The gpdb:: wrapper
 * layer calls them from C++, so nothing in SQL would otherwise reach them,
 * and the only thing that could say whether they answer correctly would be
 * the translator, which is not written yet.  These say so now, and they
 * answer "what does ORCA see about this object" on a live server.
 */

/*
 * pg_type.typname for an OID.
 *
 * Not format_type_be(): no schema qualification, and an array type is named
 * "_int4" rather than "integer[]".  ORCA keys metadata by OID and only ever
 * shows the name to someone reading a minidump.  NULL when there is no such
 * type, which is a miss in ORCA's cache rather than an error.
 */
CREATE FUNCTION gp_orca.type_name(oid)
RETURNS text
AS 'MODULE_PATHNAME', 'gp_orca_type_name'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.type_name(oid) IS
	'pg_type.typname, as ORCA reads it';

/*
 * What ORCA asks about a function before it builds metadata for it.
 *
 * "arg_types" is proargtypes, the declared input arguments.  "output_arg_types"
 * is the OUT, INOUT and TABLE arguments, and is empty for a function that
 * returns a scalar -- that is how ORCA knows whether a call yields a row.
 * "agg_transtype" is the type of an aggregate's transition state, which is
 * what travels between the halves of a split aggregate, so its width is what
 * a partial aggregate's row costs.
 */
CREATE FUNCTION gp_orca.function_fact(
	oid,
	OUT function_exists boolean,
	OUT is_aggregate boolean,
	OUT arg_types oid[],
	OUT output_arg_types oid[],
	OUT agg_transtype oid)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_function_fact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.function_fact(oid) IS
	'what the compat layer answers about a function';

/*
 * The one-argument aggregate of this name over this type, in any schema.
 *
 * ORCA looks aggregates up by name where it synthesises a call the query did
 * not write -- the count() under a split aggregate, for instance.  The lookup
 * is loose about schemas on purpose, and is safe only because the names ORCA
 * asks for are built-in ones.
 */
CREATE FUNCTION gp_orca.find_aggregate(name text, argtype oid)
RETURNS oid
AS 'MODULE_PATHNAME', 'gp_orca_find_aggregate'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.find_aggregate(text, oid) IS
	'the one-argument aggregate of this name over this type';

/*
 * Whether an implicit cast exists between two types, and what it costs.
 *
 * "binary_coercible" means the value is already in the target's
 * representation, so ORCA may put a column straight where the other type is
 * wanted.  "path_type" is how PostgreSQL would perform it, which ORCA carries
 * into DXL so the plan says the same thing.
 */
CREATE FUNCTION gp_orca.cast_fact(
	src oid,
	dst oid,
	OUT cast_exists boolean,
	OUT binary_coercible boolean,
	OUT cast_func oid,
	OUT path_type text)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_cast_fact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.cast_fact(oid, oid) IS
	'whether an implicit cast exists between two types, and what performs it';
