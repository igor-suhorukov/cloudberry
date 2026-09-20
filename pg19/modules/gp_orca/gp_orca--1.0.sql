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
 * What ORCA records about an aggregate.
 *
 * "is_ordered" is an ordered-set or hypothetical-set aggregate, which is
 * defined over the whole sorted input and so cannot be computed in halves.
 * "is_partial_capable" is whether two transition values can be merged: a
 * combine function, and serial/deserial functions when the transition value
 * is internal.  "splittable" is the conjunction ORCA acts on, and it decides
 * both whether the aggregate may be split across a Motion and whether it may
 * hash -- a hash aggregate may spill, and reading a spilled batch back is the
 * same merge.
 *
 * "is_repsafe" is whether the aggregate may be computed on a replicated
 * slice, where every segment holds the same rows and so must reach the same
 * answer.  Cloudberry keeps it in a pg_aggregate column; the port keeps it in
 * the "gp" label's replicate_safe flag, and absent means no, which is
 * Cloudberry's default too.
 *
 * NULL for an OID that is not an aggregate.
 */
CREATE FUNCTION gp_orca.aggregate_fact(
	oid,
	OUT is_ordered boolean,
	OUT is_partial_capable boolean,
	OUT is_repsafe boolean,
	OUT splittable boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_aggregate_fact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.aggregate_fact(oid) IS
	'what ORCA records about an aggregate, and what it may do with it';

/*
 * Where a function may run: 'a'ny node, the 'c'oordinator only, 'i'n an init
 * plan, or all 's'egments.
 *
 * Cloudberry reads pg_proc.proexeclocation, a column of its own; the port
 * reads the "gp" label's execute_on key, which is what EXECUTE ON becomes:
 *
 *     SECURITY LABEL FOR gp ON FUNCTION f() IS 'execute_on=all_segments'
 *
 * An unlabelled function is 'a', which is both PostgreSQL's only possible
 * answer and the only one ORCA will plan a call of: a function that has to
 * run somewhere in particular is a shape it declines.
 */
CREATE FUNCTION gp_orca.exec_location(oid)
RETURNS text
AS 'MODULE_PATHNAME', 'gp_orca_exec_location'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.exec_location(oid) IS
	'where a function may run, as the character ORCA compares against';

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

/*
 * What an operator means, and which families say so.
 *
 * ORCA does not ask "is this int4's less-than".  It asks what an operator
 * means and which operator families it belongs to, and builds metadata from
 * the answers -- which is how it reasons about types it was never compiled
 * against.  "cmptype" is eq, neq, lt, leq, gt, geq, or other for an operator
 * ORCA will not reason about.
 */
CREATE FUNCTION gp_orca.operator_fact(
	oid,
	OUT cmptype text,
	OUT opfamilies oid[])
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_operator_fact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.operator_fact(oid) IS
	'what an operator means to ORCA, and the families that say so';

/*
 * The inverse: the btree operator of this meaning over these two types.
 *
 * ORCA uses it to build a comparison the query did not write -- the equality
 * a hash join needs between two columns whose types it has just settled on.
 * NULL for "neq" and "other", because those have no btree strategy number to
 * look up.
 */
CREATE FUNCTION gp_orca.comparison_operator(
	lefttype oid,
	righttype oid,
	cmptype text)
RETURNS oid
AS 'MODULE_PATHNAME', 'gp_orca_comparison_operator'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.comparison_operator(oid, oid, text) IS
	'the btree operator of a given meaning over two types';

/*
 * The operator family of each key column of an index, in order.
 *
 * Key columns only: an INCLUDE column has no opclass.  ORCA needs this to
 * know which quals the index can answer.
 */
CREATE FUNCTION gp_orca.index_opfamilies(oid)
RETURNS oid[]
AS 'MODULE_PATHNAME', 'gp_orca_index_opfamilies'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.index_opfamilies(oid) IS
	'the operator family of each key column of an index';

/*
 * The btree family a range partition key of this type would use, or NULL when
 * the type cannot be one.
 */
CREATE FUNCTION gp_orca.default_partition_opfamily(oid)
RETURNS oid
AS 'MODULE_PATHNAME', 'gp_orca_default_partition_opfamily'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.default_partition_opfamily(oid) IS
	'the btree family a range partition key of this type would use';

/*
 * What ORCA asks about a table it is considering.
 *
 * "unique_keys" is one text array per key, because a relation's keys are not
 * all the same width and a two-dimensional SQL array would have to be
 * rectangular.  ORCA turns each into a functional dependency, which is what
 * lets it drop a grouping or prove a join does not duplicate rows.  Only
 * UNIQUE and PRIMARY KEY constraints count, and not deferrable ones -- a
 * deferrable constraint may be false in the middle of a transaction, which is
 * when a query runs.
 *
 * "check_constraints" lists only validated ones: a constraint added NOT VALID
 * may be false of rows already there, so ORCA must not reason with it.
 *
 * "has_subclass" is the exhaustive answer, not pg_class.relhassubclass, which
 * is a hint that can say yes where the answer is no.
 *
 * The two trigger columns differ in whether a partitioned table's children
 * are consulted.  ORCA does not expand children the way the Postgres planner
 * does, so it has to ask about the whole tree at once.
 */
CREATE FUNCTION gp_orca.relation_fact(
	oid,
	OUT unique_keys text[],
	OUT check_constraints oid[],
	OUT has_subclass boolean,
	OUT has_update_triggers boolean,
	OUT has_update_triggers_deep boolean,
	OUT child_distribution_mismatch boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_relation_fact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.relation_fact(oid) IS
	'what the compat layer answers about a relation';

/*
 * How ORCA is told a relation's rows are spread.
 *
 * gp.policy() answers the same question from gp_core; this reaches it the way
 * ORCA does, through relation_policy(Relation).  "kind" is named for the
 * Ereldistrpolicy the translator derives -- hash, random, replicated or
 * masteronly -- rather than for the struct it derives it from, and "attrs" is
 * the distribution key by attribute number, which is what ORCA matches
 * against a relation's columns.
 *
 * NULL for a relation with no policy, which the translator makes masteronly
 * of: all rows in one place, which is what one node means.
 */
CREATE FUNCTION gp_orca.relation_policy(
	rel regclass,
	OUT kind text,
	OUT attrs int[],
	OUT numsegments int)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_relation_policy'
LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION gp_orca.relation_policy(regclass) IS
	'how ORCA is told a relation''s rows are spread over the segments';

/*
 * What ORCA reads off a check constraint before turning it into a predicate.
 *
 * "expr" is the stored node tree, not a deparse: what matters is that a tree
 * came back and that it is the one pg_constraint holds.
 */
CREATE FUNCTION gp_orca.constraint_fact(
	oid,
	OUT name text,
	OUT relid oid,
	OUT expr text)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_constraint_fact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.constraint_fact(oid) IS
	'what the compat layer answers about a check constraint';

/*
 * The statistic kinds in the pg_statistic row ORCA would read for a column,
 * and whether that row is the inherited one.
 *
 * ORCA does not know there are two kinds of statistics.  A partitioned
 * table's useful ones are the inherited rows, which cover the children, so
 * the lookup prefers those and falls back to the non-inherited row.  NULL
 * when the column has no statistics at all.
 */
CREATE FUNCTION gp_orca.att_stats_kinds(
	relid oid,
	attnum int,
	OUT inherited boolean,
	OUT kinds int[])
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_att_stats_kinds'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.att_stats_kinds(oid, int) IS
	'the statistic kinds ORCA would read for a column';

/*
 * The extended statistics objects ORCA is told a relation has.
 *
 * One row per (object, kind, stxdinherit) triple, which is the shape the
 * translator walks.  A relation analyzed both with and without inheritance
 * reports each kind twice, differing only in "inherit"; compat/plancat.c says
 * why the port keeps that rather than deduplicating.
 */
CREATE FUNCTION gp_orca.ext_stats(
	rel regclass,
	OUT statoid oid,
	OUT name text,
	OUT kind "char",
	OUT keys int[],
	OUT inherit boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_orca_ext_stats'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.ext_stats(regclass) IS
	'the extended statistics objects ORCA is told a relation has';

/*
 * The kinds one statistics object was asked to hold.
 *
 * Asked for, not built: stxkind records what CREATE STATISTICS requested, and
 * whether ANALYZE has since produced anything is another catalog's business.
 */
CREATE FUNCTION gp_orca.ext_stats_kinds(oid)
RETURNS "char"[]
AS 'MODULE_PATHNAME', 'gp_orca_ext_stats_kinds'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.ext_stats_kinds(oid) IS
	'the kinds an extended statistics object was asked to hold';

/*
 * How big ORCA is told a partitioned table is, summed over its leaves.
 *
 * PostgreSQL's planner never asks this: it plans each partition separately
 * and adds the costs at the Append.  ORCA costs the table as one object, so
 * it needs the total before it has looked at a leaf.  On a table with no
 * children the answer is that table's own numbers.
 */
CREATE FUNCTION gp_orca.partitioned_size(
	rel regclass,
	OUT numtuples float8,
	OUT pages bigint,
	OUT allvisible_pages bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_partitioned_size'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.partitioned_size(regclass) IS
	'the rows and pages of a partitioned table, summed over its leaves';

/*
 * The resnos of every target entry computing the same expression as the one
 * at "resno".
 *
 * PostgreSQL's tlist_member() answers with the first match and stops, which
 * is right for its callers; ORCA rewrites references rather than picking one,
 * so it needs them all.  For 'SELECT a, a, b FROM t' this answers {1,2}.
 */
CREATE FUNCTION gp_orca.tlist_members(sql text, resno int DEFAULT 1)
RETURNS int[]
AS 'MODULE_PATHNAME', 'gp_orca_tlist_members'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.tlist_members(text, int) IS
	'every target entry matching one of them, not just the first';

/*
 * Which range table entries a target list refers to, before and after its
 * join alias Vars are flattened.
 *
 * A Var naming a JOIN's output column resolves only against the query that
 * owns the JOIN, and ORCA's normalization moves the target list out of it.
 * After flattening the target list names the base relations instead -- two of
 * them for a USING column, whose merged value is a COALESCE of both sides.
 *
 * "where_after" is the other half of the contract: the WHERE clause is
 * deliberately not flattened, because it does not move, and ORCA resolves its
 * alias Vars during translation instead.  "window_after" covers the frame
 * bounds, which are the one part walked by hand -- a WindowClause is not an
 * expression, so the mutator would not reach inside it.
 */
CREATE FUNCTION gp_orca.flatten_join_aliases(
	sql text,
	OUT before int[],
	OUT after int[],
	OUT where_after int[],
	OUT window_after int[])
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_flatten_join_aliases'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.flatten_join_aliases(text) IS
	'which relations a target list names, before and after flattening';

/*
 * What an array constant becomes when ORCA is given it.
 *
 * ORCA cannot look inside an array datum of a type it was never compiled
 * against, so the elements are handed to it as separate Consts inside an
 * ArrayExpr.  "in_collation" and "out_collation" are the point: they must
 * agree, or the value has changed on the way into the optimizer.
 */
CREATE FUNCTION gp_orca.array_const_to_expr(
	expr text,
	OUT kind text,
	OUT nelems int,
	OUT in_collation oid,
	OUT out_collation oid)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_array_const_to_expr'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.array_const_to_expr(text) IS
	'an array constant as the ArrayExpr ORCA can look inside';

/*
 * Would ORCA be allowed to hash this WHERE clause, if it were an ANY
 * SubLink's test expression?
 *
 * The question decides whether a subplan builds its subquery into a hash
 * table once or re-runs the comparison per outer row.  PostgreSQL asks it
 * too, and keeps the answer static, so the port re-implements it.
 *
 * The probe passes no subquery Param ids, so what it reaches is the operator
 * half of the rule: hashable, strict, binary, and with no Var of the outer
 * query on the right-hand side.
 */
CREATE FUNCTION gp_orca.testexpr_is_hashable(sql text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'gp_orca_testexpr_is_hashable'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.testexpr_is_hashable(text) IS
	'whether an ANY SubLink test expression of this shape could be hashed';

/*
 * A time-shaped constant on the one scale ORCA compares such values on:
 * microseconds since 2000-01-01 for the timestamp types, microseconds since
 * midnight for the time ones.
 *
 * "ok" is false for a type the conversion does not know, and has to be looked
 * at: 0 is a perfectly good timestamp, so the value alone cannot say.
 */
CREATE FUNCTION gp_orca.timevalue_scalar(
	expr text,
	OUT ok boolean,
	OUT value float8)
RETURNS record
AS 'MODULE_PATHNAME', 'gp_orca_timevalue_scalar'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.timevalue_scalar(text) IS
	'a time-shaped constant as the number ORCA compares it as';

/*
 * A numeric as the double ORCA holds a histogram bound in.
 *
 * "No overflow" is the point: a numeric holds values no double can, and a
 * bound that is out of range is still a usable bound once it becomes an
 * infinity.  Raising would lose the whole histogram over one bucket.
 */
CREATE FUNCTION gp_orca.numeric_scalar(numeric)
RETURNS float8
AS 'MODULE_PATHNAME', 'gp_orca_numeric_scalar'
LANGUAGE C STRICT IMMUTABLE;

COMMENT ON FUNCTION gp_orca.numeric_scalar(numeric) IS
	'a numeric as a double, saturating instead of raising';

/*
 * How many statements ORCA planned, and how many it did not and why.
 *
 * Decision 1 asks for this from the first milestone: whether to build the
 * MPP PostgreSQL planner as well as ORCA is to be decided at M7 from how
 * often the fallback fires on real workloads, and numbers that only start
 * being collected once everything works would not answer that question.
 *
 * "planned" is a row like the others, so that a reader can take the whole
 * table at one moment and work out a rate from it.  The counts belong to the
 * server rather than the session, and outlive a backend.
 */
CREATE FUNCTION gp_orca.fallbacks(
	OUT reason text,
	OUT means text,
	OUT count bigint)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gp_orca_fallbacks'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.fallbacks() IS
	'how many statements ORCA planned, and what it would not plan and why';

/*
 * Start counting again.
 *
 * Restricted, because one session resetting the counters loses every other
 * session's numbers.
 */
CREATE FUNCTION gp_orca.reset_fallbacks()
RETURNS void
AS 'MODULE_PATHNAME', 'gp_orca_reset_fallbacks'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gp_orca.reset_fallbacks() FROM PUBLIC;

COMMENT ON FUNCTION gp_orca.reset_fallbacks() IS
	'forget the plan and fallback counts, for everybody';

/*
 * The trace flags the current settings ask for.
 *
 * ORCA has no settings of its own: everything a person can turn on or off in
 * it is a bit in a set handed to the optimizer when a query is planned, and
 * every gp.optimizer_* setting adds up to this.  The ids are ORCA's own, and
 * the large ones name a transformation rule that is switched off.
 */
CREATE FUNCTION gp_orca.traceflags()
RETURNS int[]
AS 'MODULE_PATHNAME', 'gp_orca_traceflags'
LANGUAGE C STRICT;

COMMENT ON FUNCTION gp_orca.traceflags() IS
	'what the gp.optimizer_* settings add up to, in ORCA''s own terms';
