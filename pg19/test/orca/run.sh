#!/bin/bash
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
# gp_orca: ORCA is linked in, comes up, and is Cloudberry's.
#
# The module is ORCA's four core libraries compiled from Cloudberry's own tree
# at their own paths -- 920 sources, unmodified, because not one file under
# them includes a PostgreSQL header -- plus a translator under pg19/orca/ that
# is the port's own code, being the only part that knows what a PostgreSQL is.
#
# What these tests are for: to show that the ORCA the server runs is the one
# the port means to run.  A build that quietly picked up the single-node fork
# of ORCA would pass "does it load"; it would not pass "does it carry
# Cloudberry's own transformation rules".

set -u

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# gp_orca is built only where -Dorca=true, so skip rather than fail when the
# library is not there.
if [ ! -f "$("$BINDIR/pg_config" --pkglibdir)/gp_orca.so" ]; then
	echo "gp_orca was not built (-Dorca=false); skipping"
	exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cb-orca-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbo-XXXXXX)"
PORT="${PGPORT:-$((6500 + RANDOM % 200))}"
export PGPORT="$PORT" PGHOST="$SOCK"

pass=0; fail=0
ok()    { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
notok() { printf '  NOT OK %s\n' "$1"
          [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/         /'
          fail=$((fail + 1)); }

cleanup() {
	"$BINDIR/pg_ctl" -D "$WORK/data" -m immediate stop > /dev/null 2>&1
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

q() { "$PSQL" -X -q -t -A -d postgres -c "$1" 2>&1; }

is() {
	local got; got=$(q "$2")
	[ "$got" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$got]"
}

has() {
	local got; got=$(q "$2")
	case "$got" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected [$3] in [$got]" ;;
	esac
}

refused() {
	local got; got=$(q "$2")
	case "$got" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$3], got [$got]" ;;
	esac
}

echo "gp_orca: ORCA on PostgreSQL 19"
echo "  bindir $BINDIR"
echo

"$BINDIR/initdb" -D "$WORK/data" -N --locale=C --encoding=UTF8 > "$WORK/initdb.log" 2>&1 \
	|| { echo "initdb failed"; tail -20 "$WORK/initdb.log"; exit 1; }
{
	echo "unix_socket_directories = '$SOCK'"
	echo "listen_addresses = ''"
	echo "port = $PORT"
	echo "shared_preload_libraries = 'gp_core,gp_orca'"
} >> "$WORK/data/postgresql.conf"

"$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 start > /dev/null 2>&1 \
	|| { echo "server did not start"; tail -20 "$WORK/log"; exit 1; }

q "CREATE EXTENSION gp_orca CASCADE;" > /dev/null

echo "1. ORCA is linked in and comes up"

is "the module loads and reports its source" \
   "SELECT source FROM gp_orca.version();" \
   "Apache Cloudberry"

is "asking brings it up in this backend" \
   "SELECT initialized FROM gp_orca.version();" "t"

is "it carries transformation rules" \
   "SELECT xforms > 100 FROM gp_orca.version();" "t"

is "the count and the listing agree" \
   "SELECT (SELECT xforms FROM gp_orca.version()) = (SELECT count(*) FROM gp_orca.xforms());" \
   "t"

# ORCA asserts 0 < segments when it builds its cost model, and divides by the
# number in its skew model, so what gp_core reports has to be at least one even
# on a server that has no segments at all.  gp_core answered 0 once, which
# would make ORCA decline every query in this build and mis-cost in a release
# one; the load suite tests the value, and this says why ORCA cares.
is "the segment count ORCA will be handed is a usable divisor" \
   "SELECT segments >= 1 FROM gp.node();" "t"

echo
echo "2. it is Cloudberry's ORCA, not the single-node fork"

# These three rules exist only in Cloudberry's tree.  They come with the core,
# which the port takes whole, so their presence is what says the port did not
# quietly build somebody else's ORCA.
for x in CXformGet2ParallelTableScan \
         CXformPushPartialAggBelowJoin \
         CXformImplementHashSequenceProject; do
	is "$x is present" \
	   "SELECT count(*) FROM gp_orca.xforms() x WHERE x = '$x';" "1"
done

is "so are the rules every ORCA has" \
   "SELECT count(*) FROM gp_orca.xforms() x
      WHERE x IN ('CXformGet2TableScan', 'CXformLeftOuterJoin2HashJoin');" "2"

echo
echo "3. the id space has holes, and walking it does not fall into them"

# Twenty-four rules have been retired over ORCA's life.  Their ids stay in the
# enum so that the ids around them do not move -- every stored minidump
# depends on that -- and the factory has no entry for them.  ORCA's own
# Pxf() dereferences what it finds before returning it, so a walk that does
# not ask IsXformIdUsed() first takes an assert-enabled backend down.  This
# test is here because writing it that way did exactly that.
is "no rule comes back nameless" \
   "SELECT count(*) FROM gp_orca.xforms() x WHERE x IS NULL OR x = '' OR x = '?';" "0"

is "every rule is named for its class" \
   "SELECT count(*) FROM gp_orca.xforms() x WHERE x NOT LIKE 'CXform%';" "0"

is "no rule is listed twice" \
   "SELECT count(*) FROM (SELECT x FROM gp_orca.xforms() x GROUP BY x HAVING count(*) > 1) d;" \
   "0"

is "the id space is wider than the rules in it" \
   "SELECT count(*) < 177 FROM gp_orca.xforms();" "t"

echo
echo "4. ORCA comes up per backend, on demand"

# It is not brought up in _PG_init: its libraries build process-local state
# that a postmaster has no use for and that every backend would then inherit
# through fork.  The failure that would hide here is the one ORCA asserts on
# itself -- "Xform factory was already initialized" -- so what is worth
# testing is that a second backend brings it up again from nothing, and that
# one backend asking twice does not try to.
for n in 1 2 3; do
	got=$("$PSQL" -X -q -t -A -d postgres \
	      -c "SELECT xforms FROM gp_orca.version();" 2>&1)
	[ "$got" -gt 100 ] 2>/dev/null \
		&& ok "backend $n brings ORCA up from nothing" \
		|| notok "backend $n brings ORCA up from nothing" "got [$got]"
done

is "asking twice in one backend is not a second init" \
   "SELECT count(DISTINCT xforms) FROM (
      SELECT (gp_orca.version()).xforms UNION ALL
      SELECT (gp_orca.version()).xforms UNION ALL
      SELECT (gp_orca.version()).xforms) v(xforms);" "1"

is "the server carries none of ORCA's settings of its own" \
   "SELECT count(*) FROM pg_settings WHERE name LIKE 'optimizer%';" "0"

echo
echo "5. the checks in front of ORCA answer as Cloudberry's do"

# These two are the port's own copies of functions Cloudberry adds to
# PostgreSQL's optimizer/walkers.c.  The port does not build that file, so
# nothing would otherwise say that the copies answer the same way -- and one of
# them decides whether ORCA declines a query, so a wrong answer is a plan
# silently going somewhere else.
#
# has_orderby_ordering_op is the KNN shape: ORDER BY over an ordering operator
# whose argument is a plain column.  PostgreSQL's planner turns those into a
# GiST index scan and ORCA cannot, so ORCA leaves them alone.  The distinction
# it has to draw is between a bare column and a column inside a function.
q "CREATE TABLE pts (id int, p point, t text, u text COLLATE \"C\");" > /dev/null

is "ORDER BY over an ordering operator on a plain column is the KNN shape" \
   "SELECT orderby_ordering_op FROM gp_orca.explain_refusal(
      'SELECT id FROM pts ORDER BY p <-> point ''(0,0)''');" "t"

is "the same operator over a computed argument is not" \
   "SELECT orderby_ordering_op FROM gp_orca.explain_refusal(
      'SELECT id FROM pts ORDER BY center(box(p,p)) <-> point ''(0,0)''');" "f"

is "an ordinary ORDER BY is not" \
   "SELECT orderby_ordering_op FROM gp_orca.explain_refusal(
      'SELECT id FROM pts ORDER BY id');" "f"

is "and neither is no ORDER BY at all" \
   "SELECT orderby_ordering_op FROM gp_orca.explain_refusal(
      'SELECT id FROM pts');" "f"

is "nothing collatable means no non-default collation" \
   "SELECT non_default_collation FROM gp_orca.explain_refusal(
      'SELECT id FROM pts');" "f"

is "a column with the default collation does not count either" \
   "SELECT non_default_collation FROM gp_orca.explain_refusal(
      'SELECT t FROM pts');" "f"

is "a column declared COLLATE \"C\" does" \
   "SELECT non_default_collation FROM gp_orca.explain_refusal(
      'SELECT u FROM pts');" "t"

refused "more than one statement is refused rather than half read" \
        "SELECT gp_orca.explain_refusal('SELECT 1; SELECT 2');" \
        "exactly one statement"

echo
echo "6. the compat layer answers about types, functions and casts"

# pg19/orca/compat/lsyscache.c re-implements what Cloudberry adds to
# PostgreSQL's lsyscache.c.  ORCA reaches it from C++ through the gpdb::
# wrapper layer, so these probes are what lets a test see the answers before
# the translator exists to consume them.

is "typname, not the SQL spelling of a type" \
   "SELECT gp_orca.type_name('int4'::regtype);" "int4"

is "an array type is named as pg_type names it" \
   "SELECT gp_orca.type_name('int4[]'::regtype);" "_int4"

is "a type that is not there is a miss, not an error" \
   "SELECT gp_orca.type_name(0) IS NULL;" "t"

is "a function exists and is not an aggregate" \
   "SELECT function_exists AND NOT is_aggregate
      FROM gp_orca.function_fact('abs(int4)'::regprocedure);" "t"

is "and its declared argument types come back in order" \
   "SELECT arg_types::text
      FROM gp_orca.function_fact('substr(text,int4,int4)'::regprocedure);" \
   "{25,23,23}"

is "a scalar function has no output arguments" \
   "SELECT output_arg_types = '{}'::oid[]
      FROM gp_orca.function_fact('abs(int4)'::regprocedure);" "t"

# A function with OUT parameters is what tells ORCA a call yields a row.
q "CREATE FUNCTION two_out(IN a int, OUT b text, OUT c bigint)
     LANGUAGE sql AS \$\$ SELECT 'x'::text, 1::bigint \$\$;" > /dev/null

is "OUT parameters are reported, and the IN one is not" \
   "SELECT output_arg_types::text
      FROM gp_orca.function_fact('two_out(int4)'::regprocedure);" "{25,20}"

is "while arg_types still reports the input" \
   "SELECT arg_types::text
      FROM gp_orca.function_fact('two_out(int4)'::regprocedure);" "{23}"

is "an aggregate is both, and carries a transition type" \
   "SELECT is_aggregate AND agg_transtype = 'int8'::regtype::oid
      FROM gp_orca.function_fact('count(\"any\")'::regprocedure);" "t"

is "a non-aggregate has no transition type" \
   "SELECT agg_transtype IS NULL
      FROM gp_orca.function_fact('abs(int4)'::regprocedure);" "t"

is "nothing exists at OID 0" \
   "SELECT function_exists OR is_aggregate FROM gp_orca.function_fact(0);" "f"

is "an aggregate is found by name and argument type" \
   "SELECT gp_orca.find_aggregate('sum', 'int4'::regtype) =
           'sum(int4)'::regprocedure::oid;" "t"

is "the wrong argument type finds nothing" \
   "SELECT gp_orca.find_aggregate('sum', 'text'::regtype) IS NULL;" "t"

is "and a plain function is not an aggregate however it is spelled" \
   "SELECT gp_orca.find_aggregate('abs', 'int4'::regtype) IS NULL;" "t"

# What ORCA records about an aggregate, and the conjunction it acts on.  Two
# of the three answers are PostgreSQL's own columns under Cloudberry's names;
# the third, is_repsafe, is a column Cloudberry adds and the port keeps in a
# label.

is "an ordinary aggregate is not ordered, and can be computed in halves" \
   "SELECT NOT is_ordered AND is_partial_capable AND splittable
      FROM gp_orca.aggregate_fact('sum(int4)'::regprocedure);" "t"

# The case the serial/deserial branch exists for: the transition value is a
# pointer into the aggregate's own memory, so it can only travel once there
# is something to turn it into bytea and back.
is "an internal transition type is still splittable when it serialises" \
   "SELECT is_partial_capable
      FROM gp_orca.aggregate_fact('string_agg(text,text)'::regprocedure);" "t"

is "an aggregate with no combine function cannot be split" \
   "SELECT NOT is_ordered AND NOT is_partial_capable AND NOT splittable
      FROM gp_orca.aggregate_fact('xmlagg(xml)'::regprocedure);" "t"

# An ordered-set aggregate is defined over the whole sorted input, so a
# partial per segment would answer a different question.
is "an ordered-set aggregate is ordered, and so not splittable" \
   "SELECT is_ordered AND NOT splittable
      FROM gp_orca.aggregate_fact('percentile_cont(float8,float8)'::regprocedure);" "t"

# AGGKIND_IS_ORDERED_SET means "not normal", so it covers the hypothetical-set
# aggregates too -- which the name does not say and ORCA depends on.
is "and so is a hypothetical-set aggregate, which the name does not say" \
   "SELECT is_ordered FROM gp_orca.aggregate_fact('rank(\"any\")'::regprocedure);" "t"

is "a plain function is not an aggregate, and has no facts" \
   "SELECT gp_orca.aggregate_fact('abs(int4)'::regprocedure) IS NULL;" "t"

# is_repsafe is Cloudberry's pg_aggregate.aggrepsafeexec, which the port keeps
# in the "gp" label.  Absent means no, which is Cloudberry's default for the
# column, and at this milestone nothing is replicated -- so what is worth
# testing is that the label can be set at all, and on the object SECURITY
# LABEL ... ON AGGREGATE addresses.
is "no aggregate is replicate-safe until something says so" \
   "SELECT is_repsafe FROM gp_orca.aggregate_fact('sum(int4)'::regprocedure);" "f"

q "SECURITY LABEL FOR gp ON AGGREGATE sum(int4) IS 'replicate_safe';" > /dev/null

is "and labelling the aggregate is what says so" \
   "SELECT is_repsafe FROM gp_orca.aggregate_fact('sum(int4)'::regprocedure);" "t"

is "the label goes on the aggregate's pg_proc row, which is its OID" \
   "SELECT objoid = 'sum(int4)'::regprocedure::oid FROM pg_seclabel
      WHERE provider = 'gp' AND classoid = 'pg_proc'::regclass;" "t"

is "a sibling aggregate is untouched" \
   "SELECT is_repsafe FROM gp_orca.aggregate_fact('sum(int8)'::regprocedure);" "f"

q "SECURITY LABEL FOR gp ON AGGREGATE sum(int4) IS NULL;" > /dev/null

is "and removing the label puts it back" \
   "SELECT is_repsafe FROM gp_orca.aggregate_fact('sum(int4)'::regprocedure);" "f"

refused "replicate_safe is a flag, so a value is a mistake" \
   "SECURITY LABEL FOR gp ON AGGREGATE sum(int4) IS 'replicate_safe=yes';" \
   "takes no value"

# Where a function may run.  Cloudberry reads pg_proc.proexeclocation; the
# port reads the "gp" label's execute_on key, which is what EXECUTE ON
# becomes.  ORCA makes one comparison with the answer -- against ANY -- and
# declines to plan a call of anything else.
q "CREATE FUNCTION placed() RETURNS int LANGUAGE sql AS \$\$ SELECT 1 \$\$;" > /dev/null

is "an unlabelled function may run anywhere" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" "a"

is "which is the only answer ORCA will plan a call of" \
   "SELECT gp_orca.exec_location('abs(int4)'::regprocedure);" "a"

q "SECURITY LABEL FOR gp ON FUNCTION placed() IS 'execute_on=all_segments';" > /dev/null

is "EXECUTE ON ALL SEGMENTS is a label, and reads back as 's'" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" "s"

q "SECURITY LABEL FOR gp ON FUNCTION placed() IS 'execute_on=coordinator';" > /dev/null

is "the coordinator is 'c'" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" "c"

# Cloudberry's older spelling of the same place, still in its documentation,
# so a label written by hand from those docs has to mean what it says.
q "SECURITY LABEL FOR gp ON FUNCTION placed() IS 'execute_on=master';" > /dev/null

is "and so is master, which is what Cloudberry used to call it" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" "c"

q "SECURITY LABEL FOR gp ON FUNCTION placed() IS 'execute_on=initplan';" > /dev/null

is "an init plan is 'i'" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" "i"

# A typo must not read as "runs anywhere".  That is the one value that lets
# the function run everywhere, so defaulting to it would quietly undo the
# label rather than report it.
q "SECURITY LABEL FOR gp ON FUNCTION placed() IS 'execute_on=segments';" > /dev/null

refused "a value the port does not know is an error, not ANY" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" \
   "unrecognized \"execute_on\" value \"segments\""

has "and the error names the function it is on" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" "placed()"

q "SECURITY LABEL FOR gp ON FUNCTION placed() IS NULL;" > /dev/null

is "removing the label puts the function back everywhere" \
   "SELECT gp_orca.exec_location('placed()'::regprocedure);" "a"

refused "execute_on needs a value; the bare key is a mistake" \
   "SECURITY LABEL FOR gp ON FUNCTION placed() IS 'execute_on';" \
   "needs a value"

refused "and nothing is at OID 0" \
   "SELECT gp_orca.exec_location(0);" "cache lookup failed for function 0"

is "a cast that needs a function reports it" \
   "SELECT cast_exists AND NOT binary_coercible AND path_type = 'func'
      FROM gp_orca.cast_fact('int4'::regtype, 'int8'::regtype);" "t"

is "and names the function that performs it" \
   "SELECT cast_func = 'int8(int4)'::regprocedure::oid
      FROM gp_orca.cast_fact('int4'::regtype, 'int8'::regtype);" "t"

is "there is no implicit cast from text to integer" \
   "SELECT cast_exists FROM gp_orca.cast_fact('text'::regtype, 'int4'::regtype);" "f"

# The defect this fixes: Cloudberry returns from the binary-coercible branch
# without writing *pathtype, and CTranslatorRelcacheToDXL switches on the
# uninitialised value.  RELABELTYPE is the case that branch was always meant
# to take -- no function, nothing to do at run time -- and its arm in that
# switch asserts the InvalidOid this path returns.
is "a binary-coercible cast is free, and says which path it took" \
   "SELECT cast_exists AND binary_coercible AND path_type = 'relabel'
      AND cast_func IS NULL
      FROM gp_orca.cast_fact('text'::regtype, 'varchar'::regtype);" "t"

is "a type to itself is free too, by the same path" \
   "SELECT binary_coercible AND path_type = 'relabel'
      FROM gp_orca.cast_fact('int4'::regtype, 'int4'::regtype);" "t"

echo
echo "7. and about operators, families and index keys"

# get_comparison_type is the function PostgreSQL 19 changed most: Cloudberry
# reads a btree StrategyNumber out of an OpBtreeInterpretation, PG19 reads a
# CompareType out of an OpIndexInterpretation.  The switch is now over
# meanings rather than over btree's numbering, which is what ORCA wanted --
# it only ever read the strategy to recover the meaning.

is "an equality operator means equality" \
   "SELECT cmptype FROM gp_orca.operator_fact('=(int4,int4)'::regoperator);" "eq"

is "less-than means less-than" \
   "SELECT cmptype FROM gp_orca.operator_fact('<(int4,int4)'::regoperator);" "lt"

is "greater-or-equal means greater-or-equal" \
   "SELECT cmptype FROM gp_orca.operator_fact('>=(int4,int4)'::regoperator);" "geq"

# Not-equal has no btree strategy number of its own.  Cloudberry borrowed
# ROWCOMPARE_NE to say so on the way out; PG19 has COMPARE_NE as a meaning in
# its own right, so this arrives directly.
is "not-equal is a meaning even though btree has no strategy for it" \
   "SELECT cmptype FROM gp_orca.operator_fact('<>(int4,int4)'::regoperator);" "neq"

is "an operator in no index family is not a comparison" \
   "SELECT cmptype FROM gp_orca.operator_fact('+(int4,int4)'::regoperator);" "other"

# COMPARE_OVERLAP and COMPARE_CONTAINED_BY are meanings PG19 grew and ORCA has
# no counterpart for.  Cloudberry raised an error in this arm, because a btree
# strategy outside its five really was impossible; that is no longer true, so
# an unclassifiable operator is answered rather than raised.
is "a meaning ORCA has no name for is answered, not raised" \
   "SELECT cmptype FROM gp_orca.operator_fact('&&(anyrange,anyrange)'::regoperator);" \
   "other"

is "an equality operator belongs to at least one family" \
   "SELECT array_length(opfamilies, 1) >= 1
      FROM gp_orca.operator_fact('=(int4,int4)'::regoperator);" "t"

is "and the integer btree family is among them" \
   "SELECT EXISTS (
      SELECT 1
        FROM gp_orca.operator_fact('=(int4,int4)'::regoperator) o
        JOIN pg_opfamily f ON f.oid = ANY(o.opfamilies)
       WHERE f.opfname = 'integer_ops' AND f.opfmethod = 403);" "t"

is "the operator of a meaning is found from its two types" \
   "SELECT gp_orca.comparison_operator('int4'::regtype, 'int4'::regtype, 'lt')
         = '<(int4,int4)'::regoperator::oid;" "t"

is "and it round-trips with what the operator means" \
   "SELECT cmptype FROM gp_orca.operator_fact(
      gp_orca.comparison_operator('text'::regtype, 'text'::regtype, 'geq'));" "geq"

is "a cross-type comparison is found too" \
   "SELECT gp_orca.comparison_operator('int4'::regtype, 'int8'::regtype, 'eq')
         = '=(int4,int8)'::regoperator::oid;" "t"

is "not-equal cannot be built, having no btree strategy" \
   "SELECT gp_orca.comparison_operator('int4'::regtype, 'int4'::regtype, 'neq')
      IS NULL;" "t"

is "nor can a meaning ORCA does not classify" \
   "SELECT gp_orca.comparison_operator('int4'::regtype, 'int4'::regtype, 'other')
      IS NULL;" "t"

is "two types with no btree operator between them find nothing" \
   "SELECT gp_orca.comparison_operator('int4'::regtype, 'point'::regtype, 'lt')
      IS NULL;" "t"

refused "a comparison type that is not one is refused by name" \
        "SELECT gp_orca.comparison_operator('int4'::regtype, 'int4'::regtype, 'sideways');" \
        "unknown comparison type"

q "CREATE TABLE idx_probe (a int, b text, c float8);
   CREATE INDEX idx_two ON idx_probe (a, b);
   CREATE INDEX idx_incl ON idx_probe (a) INCLUDE (c);" > /dev/null

is "an index reports one family per key column, in order" \
   "SELECT array_length(gp_orca.index_opfamilies('idx_two'::regclass), 1);" "2"

is "and they are the families of the key columns' types" \
   "SELECT gp_orca.index_opfamilies('idx_two'::regclass)
         = ARRAY[(SELECT opcfamily FROM pg_opclass
                   WHERE opcname = 'int4_ops' AND opcmethod = 403),
                 (SELECT opcfamily FROM pg_opclass
                   WHERE opcname = 'text_ops' AND opcmethod = 403)];" "t"

# An INCLUDE column carries no opclass, so indnkeyatts is the bound and not
# indnatts.  Getting that wrong would read past the opclass vector.
is "an INCLUDE column is not a key column and has no family" \
   "SELECT array_length(gp_orca.index_opfamilies('idx_incl'::regclass), 1);" "1"

is "a sortable type can be a range partition key" \
   "SELECT gp_orca.default_partition_opfamily('int4'::regtype) IS NOT NULL;" "t"

is "and text can too" \
   "SELECT gp_orca.default_partition_opfamily('text'::regtype) IS NOT NULL;" "t"

is "a type with no btree ordering cannot" \
   "SELECT gp_orca.default_partition_opfamily('point'::regtype) IS NULL;" "t"

echo
echo "8. and about constraints, keys, statistics and triggers"

q "CREATE TABLE rel_probe (
     a int PRIMARY KEY,
     b int,
     c int,
     d int,
     CONSTRAINT b_pos CHECK (b > 0),
     CONSTRAINT bc_uniq UNIQUE (b, c),
     CONSTRAINT d_def UNIQUE (d) DEFERRABLE);
   ALTER TABLE rel_probe ADD CONSTRAINT c_pos CHECK (c > 0) NOT VALID;
   INSERT INTO rel_probe SELECT g, g, g, g FROM generate_series(1, 200) g;
   ANALYZE rel_probe;" > /dev/null

is "a primary key is a unique key" \
   "SELECT '{1}' = ANY(unique_keys) FROM gp_orca.relation_fact('rel_probe'::regclass);" "t"

is "and so is a multi-column UNIQUE constraint, in column order" \
   "SELECT '{2,3}' = ANY(unique_keys) FROM gp_orca.relation_fact('rel_probe'::regclass);" "t"

# A deferrable constraint may be false in the middle of a transaction, which
# is exactly when a query runs, so it promises nothing to the optimizer.
is "a deferrable unique constraint is not a key" \
   "SELECT '{4}' = ANY(unique_keys) FROM gp_orca.relation_fact('rel_probe'::regclass);" "f"

is "so two keys are reported, not three" \
   "SELECT array_length(unique_keys, 1) FROM gp_orca.relation_fact('rel_probe'::regclass);" "2"

# NOT VALID means the constraint may be false of rows already there.  ORCA
# would prune with it, so it must not see it.
is "only the validated check constraint is reported" \
   "SELECT array_length(check_constraints, 1)
      FROM gp_orca.relation_fact('rel_probe'::regclass);" "1"

is "and it is the one that was validated" \
   "SELECT name FROM gp_orca.constraint_fact(
      (SELECT check_constraints[1] FROM gp_orca.relation_fact('rel_probe'::regclass)));" \
   "b_pos"

q "ALTER TABLE rel_probe VALIDATE CONSTRAINT c_pos;" > /dev/null

is "validating the other one makes it visible too" \
   "SELECT array_length(check_constraints, 1)
      FROM gp_orca.relation_fact('rel_probe'::regclass);" "2"

is "a check constraint names its relation" \
   "SELECT relid = 'rel_probe'::regclass FROM gp_orca.constraint_fact(
      (SELECT oid FROM pg_constraint WHERE conname = 'b_pos'));" "t"

is "and hands back the stored expression tree" \
   "SELECT expr LIKE '%OPEXPR%' FROM gp_orca.constraint_fact(
      (SELECT oid FROM pg_constraint WHERE conname = 'b_pos'));" "t"

# A unique constraint has no conbin, and ORCA only ever asks this of OIDs
# get_check_constraint_oids() gave it, so a null expression is not an error.
is "a constraint with no expression has none, rather than failing" \
   "SELECT expr IS NULL FROM gp_orca.constraint_fact(
      (SELECT oid FROM pg_constraint WHERE conname = 'bc_uniq'));" "t"

is "a constraint that is not there is all nulls" \
   "SELECT name IS NULL AND relid IS NULL AND expr IS NULL
      FROM gp_orca.constraint_fact(0);" "t"

is "an analyzed column has statistics" \
   "SELECT kinds IS NOT NULL FROM gp_orca.att_stats_kinds('rel_probe'::regclass, 1);" "t"

is "and among them the one ORCA needs most, a histogram" \
   "SELECT 2 = ANY(kinds) FROM gp_orca.att_stats_kinds('rel_probe'::regclass, 1);" "t"

is "a plain table's statistics are not the inherited ones" \
   "SELECT inherited FROM gp_orca.att_stats_kinds('rel_probe'::regclass, 1);" "f"

is "a column never analyzed has none" \
   "SELECT gp_orca.att_stats_kinds('rel_probe'::regclass, 5) IS NULL;" "t"

q "CREATE TABLE parent_probe (a int, b int) PARTITION BY RANGE (a);
   CREATE TABLE child_probe PARTITION OF parent_probe FOR VALUES FROM (1) TO (100);
   INSERT INTO parent_probe SELECT g, g FROM generate_series(1, 99) g;
   ANALYZE parent_probe;" > /dev/null

# A partitioned table's own row is empty; the statistics that describe its
# data are the inherited ones.  ORCA does not know there are two kinds, so
# asking for inherited first is what makes one call serve both.
is "a partitioned table's statistics are the inherited ones" \
   "SELECT inherited FROM gp_orca.att_stats_kinds('parent_probe'::regclass, 1);" "t"

is "a parent with a partition really has a subclass" \
   "SELECT has_subclass FROM gp_orca.relation_fact('parent_probe'::regclass);" "t"

is "and a table with none does not" \
   "SELECT has_subclass FROM gp_orca.relation_fact('rel_probe'::regclass);" "f"

q "CREATE FUNCTION noop_trig() RETURNS trigger LANGUAGE plpgsql
     AS \$\$ BEGIN RETURN NEW; END \$\$;
   CREATE TRIGGER t_upd BEFORE UPDATE ON child_probe
     FOR EACH ROW EXECUTE FUNCTION noop_trig();" > /dev/null

# ORCA asks before planning an update that moves a row: a split update is a
# delete and an insert, which would fire the wrong triggers.
q "CREATE TABLE bare_probe (a int, b int);" > /dev/null

is "a table with no triggers has no update triggers" \
   "SELECT has_update_triggers FROM gp_orca.relation_fact('bare_probe'::regclass);" "f"

# Found by a test that asserted the opposite of the truth about its own
# fixture.  A DEFERRABLE unique constraint creates an internal
# Unique_ConstraintTrigger whose tgtype is row|insert|update, so a table
# nobody put a trigger on reports an update trigger and ORCA will not split
# an update on it.  Cloudberry behaves the same way; what is new here is that
# it is written down.
is "but a deferrable unique constraint quietly makes one" \
   "SELECT has_update_triggers FROM gp_orca.relation_fact('rel_probe'::regclass);" "t"

is "and it is internal, not something the user created" \
   "SELECT bool_and(tgisinternal) FROM pg_trigger
      WHERE tgrelid = 'rel_probe'::regclass;" "t"

is "a child's trigger is not the parent's own" \
   "SELECT has_update_triggers FROM gp_orca.relation_fact('parent_probe'::regclass);" "f"

# This is the asymmetry the "including_children" argument exists for: the
# Postgres planner sees each leaf and asks about each, ORCA sees the parent.
is "but it is found when the whole partition tree is asked about" \
   "SELECT has_update_triggers_deep FROM gp_orca.relation_fact('parent_probe'::regclass);" "t"

q "ALTER TABLE child_probe DISABLE TRIGGER t_upd;" > /dev/null

is "a disabled trigger would not fire, so it does not count" \
   "SELECT has_update_triggers_deep FROM gp_orca.relation_fact('parent_probe'::regclass);" "f"

###############################################################################
echo
echo "9. and about distribution, which its metadata asks even on one node"
###############################################################################
# Nothing about ORCA's metadata is single-node: its relcache translator asks
# every relation it sees how its rows are spread, so the port needs this at M1
# rather than at the milestone that has segments to spread over.
#
# The labels are set directly here rather than through DISTRIBUTED BY,
# because this suite preloads gp_core and gp_orca and not gp_sql.  That is
# also the harder test: the reader cannot lean on the writer having checked
# the shape.

q "CREATE TABLE dist_none (a int, b int);
   CREATE TABLE dist_rand (a int, b int);
   CREATE TABLE dist_repl (a int, b int);
   CREATE TABLE dist_hash (a int, b int);
   SECURITY LABEL FOR gp ON TABLE dist_rand IS 'distributed_by=random';
   SECURITY LABEL FOR gp ON TABLE dist_repl IS 'distributed_by=replicated';
   SECURITY LABEL FOR gp ON TABLE dist_hash IS 'distributed_by=\"(a,b)\"';" > /dev/null

# The common answer, and the one that needed no decision: a null policy is
# what the translator makes EreldistrMasterOnly of, which is all rows in one
# place -- what one node means.
is "a relation nobody distributed has no policy" \
   "SELECT gp_orca.relation_policy('dist_none'::regclass) IS NULL;" "t"

is "a random policy has no key" \
   "SELECT kind || ' ' || attrs::text
      FROM gp_orca.relation_policy('dist_rand'::regclass);" "random {}"

is "a replicated one says so" \
   "SELECT kind FROM gp_orca.relation_policy('dist_repl'::regclass);" "replicated"

# ORCA matches these attribute numbers against the relation's columns, and
# asserts "Column not found" if one is not there.
is "and a hash policy carries the key by attribute number" \
   "SELECT kind || ' ' || attrs::text
      FROM gp_orca.relation_policy('dist_hash'::regclass);" "hash {1,2}"

is "the policy carries a segment count ORCA can divide by" \
   "SELECT numsegments >= 1 FROM gp_orca.relation_policy('dist_hash'::regclass);" "t"

# child_distribution_mismatch: the one mismatch Cloudberry allows is a
# hash-distributed root with a randomly distributed part, and ORCA has to know
# because a scan of the tree cannot then claim the root's hash distribution.
q "CREATE TABLE dist_root (a int, b int) PARTITION BY RANGE (a);
   CREATE TABLE dist_p1 PARTITION OF dist_root FOR VALUES FROM (1) TO (10);
   CREATE TABLE dist_p2 PARTITION OF dist_root FOR VALUES FROM (10) TO (20);" > /dev/null

# Cloudberry asserts a partitioned table has a policy at all -- its DDL sees
# to that.  The port reaches an unlabelled partitioned table constantly, and
# a root whose rows are all in one place has nothing a child could differ
# from.
is "an unlabelled partitioned table has no mismatch to report" \
   "SELECT child_distribution_mismatch
      FROM gp_orca.relation_fact('dist_root'::regclass);" "f"

q "SECURITY LABEL FOR gp ON TABLE dist_root IS 'distributed_by=\"(a)\"';
   SECURITY LABEL FOR gp ON TABLE dist_p1 IS 'distributed_by=\"(a)\"';
   SECURITY LABEL FOR gp ON TABLE dist_p2 IS 'distributed_by=\"(a)\"';" > /dev/null

is "nor does one whose parts all match the root" \
   "SELECT child_distribution_mismatch
      FROM gp_orca.relation_fact('dist_root'::regclass);" "f"

q "SECURITY LABEL FOR gp ON TABLE dist_p2 IS 'distributed_by=random';" > /dev/null

is "a random part under a hashed root is the mismatch" \
   "SELECT child_distribution_mismatch
      FROM gp_orca.relation_fact('dist_root'::regclass);" "t"

# A random root is already as unconstrained as a child can be, so no child can
# differ from it -- which is why Cloudberry returns early there too.
q "SECURITY LABEL FOR gp ON TABLE dist_root IS 'distributed_by=random';" > /dev/null

is "but under a random root there is nothing to differ from" \
   "SELECT child_distribution_mismatch
      FROM gp_orca.relation_fact('dist_root'::regclass);" "f"

is "and a table that is not partitioned never has one" \
   "SELECT child_distribution_mismatch
      FROM gp_orca.relation_fact('dist_hash'::regclass);" "f"

# The reader raises rather than shrugs, because a policy read wrong is a plan
# built on the wrong distribution.  gp_sql.set_distribution() refuses these
# shapes; SECURITY LABEL will take them, which is why the reader checks too.
q "SECURITY LABEL FOR gp ON TABLE dist_none IS 'distributed_by=sideways';" > /dev/null

refused "a shape the port does not know is an error" \
        "SELECT kind FROM gp_orca.relation_policy('dist_none'::regclass);" \
        "unrecognized distribution policy \"sideways\""

q "SECURITY LABEL FOR gp ON TABLE dist_none IS 'distributed_by=\"(nosuch)\"';" > /dev/null

refused "and a key column that is not there is named" \
        "SELECT kind FROM gp_orca.relation_policy('dist_none'::regclass);" \
        "column \"nosuch\" of the distribution policy of \"dist_none\" does not exist"


echo
echo "10. extended statistics, as ORCA is told about them"

q "CREATE TABLE es (a int, b int, c text);
   INSERT INTO es SELECT i % 10, i % 5, 'x' FROM generate_series(1, 1000) i;
   CREATE STATISTICS es_stx (ndistinct, dependencies) ON a, b FROM es;" > /dev/null

# stxkind is what CREATE STATISTICS asked for.  pg_statistic_ext_data is what
# ANALYZE has since produced, and the two are different questions -- which is
# the distinction the two functions draw.
is "the kinds asked for are readable before anything is built" \
   "SELECT array_length(gp_orca.ext_stats_kinds(oid), 1)
      FROM pg_statistic_ext WHERE stxname = 'es_stx';" "2"

is "and they are the two that were asked for" \
   "SELECT gp_orca.ext_stats_kinds(oid) <@ '{d,f}'::\"char\"[]
       AND gp_orca.ext_stats_kinds(oid) @> '{d,f}'::\"char\"[]
      FROM pg_statistic_ext WHERE stxname = 'es_stx';" "t"

is "an object nobody has analyzed reports nothing built" \
   "SELECT count(*) FROM gp_orca.ext_stats('es'::regclass);" "0"

q "ANALYZE es;" > /dev/null

is "after ANALYZE both kinds are there" \
   "SELECT count(*) FROM gp_orca.ext_stats('es'::regclass);" "2"

is "each names the object it came from" \
   "SELECT count(DISTINCT name) || ' ' || min(name)
      FROM gp_orca.ext_stats('es'::regclass);" "1 es_stx"

# The columns an object covers are what ORCA matches against a relation's
# attributes, so they are reported by attribute number, as ORCA reads them.
is "and the columns it covers, by attribute number" \
   "SELECT DISTINCT keys::text FROM gp_orca.ext_stats('es'::regclass);" "{1,2}"

is "a plain table has no inherited row, so nothing is reported twice" \
   "SELECT count(*) FROM gp_orca.ext_stats('es'::regclass) WHERE inherit;" "0"

# GetExtStatisticsName used to read its tuple after releasing the syscache
# entry it came from.  Nothing here can observe a use-after-release directly;
# what it can observe is that the name comes back at all, for every row.
is "the name survives the lookup that produced it" \
   "SELECT count(*) FROM gp_orca.ext_stats('es'::regclass) WHERE name = 'es_stx';" "2"

is "a relation with no statistics objects reports none" \
   "SELECT count(*) FROM gp_orca.ext_stats('dist_hash'::regclass);" "0"

echo
echo "11. how big a partitioned table is, summed over its leaves"

q "CREATE TABLE ps (a int) PARTITION BY RANGE (a);
   CREATE TABLE ps1 PARTITION OF ps FOR VALUES FROM (0) TO (100);
   CREATE TABLE ps2 PARTITION OF ps FOR VALUES FROM (100) TO (200);
   INSERT INTO ps SELECT i FROM generate_series(0, 199) i;" > /dev/null

# THE DEFECT THIS EXISTS TO CATCH.  Since PostgreSQL 14 an unanalyzed relation
# has reltuples = -1, meaning "unknown".  Cloudberry adds that -1 into the
# total, so a partitioned table whose leaves are all fresh reports a negative
# row count to the optimizer -- a root and two leaves make -3.  Unknown
# contributes nothing to a sum.
is "a table nobody has analyzed reports no rows, not minus one per leaf" \
   "SELECT numtuples FROM gp_orca.partitioned_size('ps'::regclass);" "0"

is "and no pages" \
   "SELECT pages FROM gp_orca.partitioned_size('ps'::regclass);" "0"

q "ANALYZE ps1; ANALYZE ps2;" > /dev/null

# The root is still unanalyzed, so this is the summing path: PostgreSQL's
# planner would never ask, because it costs each partition on its own.
is "leaves that have been analyzed are summed" \
   "SELECT numtuples FROM gp_orca.partitioned_size('ps'::regclass);" "200"

is "and so are their pages" \
   "SELECT pages = (SELECT sum(relpages) FROM pg_class
                     WHERE relname IN ('ps1', 'ps2'))
      FROM gp_orca.partitioned_size('ps'::regclass);" "t"

q "ANALYZE ps;" > /dev/null

# Once the root has numbers of its own they are the answer, and no leaf is
# opened at all.
is "a root with numbers of its own answers from them" \
   "SELECT numtuples FROM gp_orca.partitioned_size('ps'::regclass);" "200"

is "a table with no children reports its own numbers" \
   "SELECT numtuples FROM gp_orca.partitioned_size('es'::regclass);" "1000"

# gp.enable_relsize_collection means "go and ask how big it really is".  On a
# cluster that is a dispatch to the segments; on one node this backend can
# read the relation, which is the same answer.  M2 is where the two part.
q "CREATE TABLE ps_fresh (a int) PARTITION BY RANGE (a);
   CREATE TABLE ps_fresh1 PARTITION OF ps_fresh FOR VALUES FROM (0) TO (100);
   INSERT INTO ps_fresh SELECT i FROM generate_series(0, 99) i;" > /dev/null

is "with relsize collection off, an unanalyzed leaf stays unknown" \
   "SET gp.enable_relsize_collection = off;
    SELECT numtuples FROM gp_orca.partitioned_size('ps_fresh'::regclass);" "0"

is "with it on, the relation itself is asked" \
   "SET gp.enable_relsize_collection = on;
    SELECT numtuples > 0 FROM gp_orca.partitioned_size('ps_fresh'::regclass);" "t"

echo
echo "12. every target entry that computes an expression, not just the first"

# PostgreSQL's tlist_member() answers with the first match and stops, which is
# right for its callers: they want *a* place the expression is computed.  ORCA
# rewrites references rather than picking one, so leaving the second
# occurrence pointing at the first one's column would change what the plan
# projects.
is "a column named twice is found twice" \
   "SELECT gp_orca.tlist_members('SELECT a, a, b FROM es', 1)::text;" "{1,2}"

is "a column named once is found once" \
   "SELECT gp_orca.tlist_members('SELECT a, a, b FROM es', 3)::text;" "{3}"

is "and it matches on the expression, not on the column" \
   "SELECT gp_orca.tlist_members('SELECT a + 1, b, a + 1 FROM es', 1)::text;" "{1,3}"

is "an expression that differs is not a match" \
   "SELECT gp_orca.tlist_members('SELECT a + 1, a + 2 FROM es', 1)::text;" "{1}"

refused "a resno that is not there says so" \
        "SELECT gp_orca.tlist_members('SELECT a FROM es', 7);" \
        "no target entry with resno 7"

echo
echo "13. join alias Vars, flattened where they move and left where they do not"

q "CREATE TABLE ja1 (x int, y int);
   CREATE TABLE ja2 (x int, z int);" > /dev/null

# A Var naming a JOIN's output column resolves only against the query that
# owns the JOIN, and ORCA's normalization moves the target list out of that
# query -- so it has to name the base relations by then.
#
# WHERE THAT ACTUALLY HAPPENS, which is narrower than it sounds.  A USING
# column only stays pointed at the join when the join is a FULL OUTER one,
# because only then is the merged value COALESCE(a.x, b.x) rather than one of
# the inputs.  That is why one Var becomes two here.
is "a FULL JOIN's USING column names the join before, and both sides after" \
   "SELECT before::text || ' -> ' || after::text
      FROM gp_orca.flatten_join_aliases(
        'SELECT x FROM ja1 FULL JOIN ja2 USING (x)');" "{3} -> {1,2}"

# And the other side of that: an inner join's merged column is exactly the
# left input, so the parser resolves it while analyzing and there is nothing
# left to flatten.  This is worth a test because it is easy to assume
# otherwise, and because it says how rarely the function does anything.
is "an inner join's USING column was already resolved by the parser" \
   "SELECT before::text || ' -> ' || after::text
      FROM gp_orca.flatten_join_aliases(
        'SELECT x FROM ja1 JOIN ja2 USING (x)');" "{1} -> {1}"

is "and so was a LEFT JOIN's" \
   "SELECT before::text || ' -> ' || after::text
      FROM gp_orca.flatten_join_aliases(
        'SELECT x FROM ja1 LEFT JOIN ja2 USING (x)');" "{1} -> {1}"

is "a query with no join is left alone" \
   "SELECT before::text || ' -> ' || after::text
      FROM gp_orca.flatten_join_aliases('SELECT x FROM ja1');" "{1} -> {1}"

# A whole-row reference to a join is the other case that has to be expanded:
# there is no such row on disk, so it becomes a RowExpr naming the inputs.
is "a whole-row reference to a join becomes its inputs" \
   "SELECT before::text || ' -> ' || after::text
      FROM gp_orca.flatten_join_aliases(
        'SELECT ROW(j) FROM (ja1 FULL JOIN ja2 USING (x)) j');" "{3} -> {1,2}"

# The other half of the contract, and the half that would fail silently: the
# WHERE clause is deliberately not flattened.  It does not move, and ORCA
# resolves its alias Vars during translation through its own <query level,
# varno, varattno> mapping.  Flattening it too would look like an improvement.
is "the WHERE clause still names the join afterwards" \
   "SELECT where_after::text
      FROM gp_orca.flatten_join_aliases(
        'SELECT y FROM ja1 FULL JOIN ja2 USING (x) WHERE x > 0');" "{3}"

# The frame-bound loop cannot fire, and this is why: PostgreSQL will not
# accept a frame offset that names a column at the query's own level.  The
# assertion is on that rule rather than on the loop, so if the rule is ever
# relaxed this is what says to go and look at the loop.
refused "a frame bound may not name a column at all" \
        "SELECT window_after FROM gp_orca.flatten_join_aliases(
           'SELECT count(*) OVER (ORDER BY y ROWS BETWEEN x PRECEDING AND CURRENT ROW)
              FROM ja1');" \
        "argument of ROWS must not contain variables"

is "so a frame bound that is accepted has nothing to flatten" \
   "SELECT window_after::text FROM gp_orca.flatten_join_aliases(
      'SELECT x, count(*) OVER (ORDER BY y ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
         FROM ja1 FULL JOIN ja2 USING (x)');" "{}"
echo
echo "14. an array constant ORCA can look inside"

# ORCA derives constraints from an IN list by reading the elements out.  It
# cannot look inside an array datum of a type it was never compiled against,
# so the elements are handed to it as separate Consts.
is "an array constant becomes an expression with its elements" \
   "SELECT kind || ' ' || nelems
      FROM gp_orca.array_const_to_expr('''{1,2,3}''::int[]');" "ArrayExpr 3"

is "an empty array is still an expression" \
   "SELECT kind || ' ' || nelems
      FROM gp_orca.array_const_to_expr('''{}''::int[]');" "ArrayExpr 0"

is "something that is not an array comes back unchanged" \
   "SELECT kind FROM gp_orca.array_const_to_expr('42');" "Const"

is "and so does a NULL of array type, which has no elements" \
   "SELECT kind FROM gp_orca.array_const_to_expr('NULL::int[]');" "Const"

# THE POSTGRESQL 19 DIFFERENCE.  ArrayExpr grew array_collid after Cloudberry
# forked, and Cloudberry's rewrite does not set it -- so on Cloudberry a
# text[] constant arrives at the optimizer with no collation at all, and
# exprCollation() of the rewritten expression disagrees with the Const it
# replaced.
is "a collatable array keeps its collation" \
   "SELECT in_collation = out_collation
      FROM gp_orca.array_const_to_expr('''{a,b}''::text[]');" "t"

is "and that collation is a real one, so the check has teeth" \
   "SELECT out_collation <> 0
      FROM gp_orca.array_const_to_expr('''{a,b}''::text[]');" "t"

is "a non-collatable array has none either way" \
   "SELECT in_collation = 0 AND out_collation = 0
      FROM gp_orca.array_const_to_expr('''{1,2}''::int[]');" "t"
echo
echo "15. two more files the compat layer needs than the plan said"

# cloudberry.md records, under "Corrections to the plan", that selfuncs.c and
# subselect.c "are not part of the compat layer" because the functions ORCA
# calls from them exist in PostgreSQL 19 unchanged.  They do exist -- and they
# are static, so nothing outside their own file can call them.  Cloudberry's
# whole contribution to both files is to delete the word "static".  The check
# that produced the wrong answer looked for the function in PostgreSQL's
# sources; what decides it is whether a header declares it.

# A hashable, strict, binary operator with a constant on the right: the shape
# a hashed subplan needs.
is "an equality against a constant could be hashed" \
   "SELECT gp_orca.testexpr_is_hashable('SELECT 1 FROM es WHERE a = 1');" "t"

# A Var of the outer query on the right-hand side is exactly what the rule
# forbids: that side belongs to the subquery.
is "but not one whose right-hand side names a column" \
   "SELECT gp_orca.testexpr_is_hashable('SELECT 1 FROM es WHERE a = b');" "f"

is "an operator that cannot hash is refused" \
   "SELECT gp_orca.testexpr_is_hashable('SELECT 1 FROM es WHERE a < 1');" "f"

is "an AND of hashable comparisons is hashable" \
   "SELECT gp_orca.testexpr_is_hashable('SELECT 1 FROM es WHERE a = 1 AND b = 2');" "t"

is "an OR of them is not, because it is not an AND-clause" \
   "SELECT gp_orca.testexpr_is_hashable('SELECT 1 FROM es WHERE a = 1 OR b = 2');" "f"

is "and neither is an AND with something else in it" \
   "SELECT gp_orca.testexpr_is_hashable('SELECT 1 FROM es WHERE a = 1 AND b < 2');" "f"

is "a text equality is, and collation does not change that" \
   "SELECT gp_orca.testexpr_is_hashable('SELECT 1 FROM es WHERE c = ''x''');" "t"

# The scale is PostgreSQL's own: microseconds since 2000-01-01 for the
# timestamp family.  What matters to ORCA is not the origin but that every
# value of one type lands on the same one.
is "a timestamp is microseconds since 2000-01-01" \
   "SELECT value FROM gp_orca.timevalue_scalar('timestamp ''2000-01-02 00:00:00''');" \
   "86400000000"

is "a date lands on the same scale as a timestamp" \
   "SELECT value FROM gp_orca.timevalue_scalar('date ''2000-01-02''');" "86400000000"

is "a timestamptz too" \
   "SET TimeZone = 'UTC';
    SELECT value FROM gp_orca.timevalue_scalar('timestamptz ''2000-01-02 00:00:00+00''');" \
   "86400000000"

is "a time is microseconds since midnight" \
   "SELECT value FROM gp_orca.timevalue_scalar('time ''01:00:00''');" "3600000000"

# An interval's months are days of an average month, because a month has no
# fixed length: not accurate, and not meant to be.
is "an interval of a day is a day" \
   "SELECT value FROM gp_orca.timevalue_scalar('interval ''1 day''');" "86400000000"

is "and one of a month is an average month" \
   "SELECT round((value / 86400000000.0)::numeric, 4)
      FROM gp_orca.timevalue_scalar('interval ''1 month''');" "30.4375"

# The failure flag has to be looked at: 0 is a perfectly good timestamp, so
# the value alone cannot say that the type was not understood.
is "a type it does not know says so rather than guessing" \
   "SELECT ok FROM gp_orca.timevalue_scalar('42');" "f"

is "and a type it does know says so too" \
   "SELECT ok FROM gp_orca.timevalue_scalar('date ''2000-01-01''');" "t"

# "No overflow" is the whole reason this is not just a cast: a numeric holds
# values no double can, and a histogram bound that is out of range is still a
# usable bound once it is an infinity.
is "an ordinary numeric converts" \
   "SELECT gp_orca.numeric_scalar(1.5);" "1.5"

is "a negative one too" \
   "SELECT gp_orca.numeric_scalar(-2.25);" "-2.25"

is "one no double can hold saturates instead of raising" \
   "SELECT gp_orca.numeric_scalar(('1' || repeat('0', 400))::numeric);" "Infinity"

is "and so does its negative" \
   "SELECT gp_orca.numeric_scalar(('-1' || repeat('0', 400))::numeric);" "-Infinity"

refused "a plain cast would have raised instead" \
        "SELECT ('1' || repeat('0', 400))::numeric::float8;" \
        "is out of range for type double precision"

echo
echo "16. planner_hook, and the count of what ORCA would not plan"

# Decision 1 asks for these counters "from the first milestone, on real
# workloads": whether to build Route B as well is decided at M7 from how
# often the fallback fires and why, and numbers that only start when
# everything works would not answer that.  At M1 the reason is always the
# same one -- there is no translator yet -- and what is being tested is that
# the machinery around it reports the truth.

q "SELECT gp_orca.reset_fallbacks();" > /dev/null

is "every reason is listed, plus the plans ORCA made" \
   "SELECT count(*) > 1 FROM gp_orca.fallbacks();" "t"

is "and each one says what it means" \
   "SELECT count(*) FROM gp_orca.fallbacks() WHERE means IS NULL OR means = '';" "0"

is "nothing is counted twice" \
   "SELECT count(*) FROM (SELECT reason FROM gp_orca.fallbacks()
                           GROUP BY reason HAVING count(*) > 1) d;" "0"

# The hook is installed, so a statement that reaches the planner is counted.
# Reading the counter is itself a statement, which is why the check is that
# it moved rather than that it holds a particular number.
q "SELECT gp_orca.reset_fallbacks();" > /dev/null
before=$(q "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'no_translator';")
q "SELECT 1 FROM es WHERE a = 1;" > /dev/null
after=$(q "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'no_translator';")
[ "$after" -gt "$before" ] \
	&& ok "a query that ORCA cannot plan yet is counted, with the reason" \
	|| notok "a query that ORCA cannot plan yet is counted, with the reason" \
	         "before [$before], after [$after]"

# Nothing is planned by ORCA yet, and the counter says so rather than being
# quietly absent.  This is the assertion that has to change when the
# translator lands.
is "and nothing has been planned by ORCA" \
   "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'planned';" "0"

# gp.optimizer off is a different reason from "could not", and telling them
# apart is the point of counting reasons rather than a single total.
q "SELECT gp_orca.reset_fallbacks();" > /dev/null
q "SET gp.optimizer = off; SELECT 1 FROM es WHERE a = 1;" > /dev/null
is "a session with the optimizer off reports that, not a failure" \
   "SELECT count > 0 FROM gp_orca.fallbacks() WHERE reason = 'disabled';" "t"

# A utility statement is not a query ORCA plans, and it reaches the planner
# only through the paths that wrap one.
q "SELECT gp_orca.reset_fallbacks();" > /dev/null
q "SELECT 1 FROM es WHERE a = 1;" > /dev/null
is "an ordinary query is not counted as a utility statement" \
   "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'utility';" "0"

# The counters are the server's, not the session's: a second backend sees
# what the first one did.  That is what makes them answer a question about a
# workload rather than about one connection.
q "SELECT gp_orca.reset_fallbacks();" > /dev/null
"$PSQL" -X -q -t -A -d postgres -c "SELECT 1 FROM es WHERE a = 1;" > /dev/null 2>&1
is "and another backend's fallbacks are visible from this one" \
   "SELECT count > 0 FROM gp_orca.fallbacks() WHERE reason = 'no_translator';" "t"

# Resetting is restricted, because one session doing it loses everybody
# else's numbers.
is "resetting is not something every user may do" \
   "SELECT has_function_privilege('public', 'gp_orca.reset_fallbacks()', 'execute');" "f"

# The plan still comes out, and is PostgreSQL's.  A hook that counted and
# then lost the plan would pass every test above.
is "the fallback still returns a plan, and it runs" \
   "SELECT count(*) FROM es WHERE a = 1;" "100"

is "and EXPLAIN shows PostgreSQL's plan for it" \
   "SELECT count(*) FROM (
      SELECT * FROM (VALUES (1)) v) t;" "1"

echo
echo "17. ORCA's settings, and what they add up to"

# ORCA has no settings of its own.  Everything a person can turn on or off in
# it is a bit in a set handed to the optimizer when a query is planned, and
# config/CConfigParamMapping.cpp is where the outside becomes the inside.
# Until there is a translator to hand that set to, this is the only way to
# see that the settings reach ORCA at all.

is "the settings are there, under names PostgreSQL 19 will take" \
   "SELECT count(*) > 70 FROM pg_settings WHERE name LIKE 'gp.optimizer%';" "t"

# All 438 of Cloudberry's settings change name, not only the ones written to
# a file: PostgreSQL will not define a custom variable without a dot in its
# name.  This is the correction cloudberry.md records, and it is checkable.
is "and Cloudberry's undotted spelling is not one of them" \
   "SELECT count(*) FROM pg_settings WHERE name = 'optimizer_join_order';" "0"

is "the enum settings take Cloudberry's words" \
   "SET gp.optimizer_join_order = 'greedy';
    SELECT setting FROM pg_settings WHERE name = 'gp.optimizer_join_order';" "greedy"

refused "and refuse a word that is not one of them" \
        "SET gp.optimizer_join_order = 'sideways';" \
        "invalid value for parameter"

is "the strategy setting keeps its range" \
   "RESET gp.optimizer_join_order;
    SELECT max_val FROM pg_settings WHERE name = 'gp.optimizer_agg_pds_strategy';" "3"

# The defaults already ask for flags: two are set unconditionally, and the
# join-order heuristic contributes a set of its own.
is "the default settings ask for trace flags" \
   "SELECT array_length(gp_orca.traceflags(), 1) > 0;" "t"

# Turning a scan off adds exactly the two rules that produce it -- the plain
# one and Cloudberry's parallel one, which comes with ORCA's core whether or
# not the port plans with it.
is "switching table scans off adds exactly two rules" \
   "SELECT array_length(t2.f, 1) - array_length(t1.f, 1) FROM
      (SELECT gp_orca.traceflags() AS f) t1,
      LATERAL (SELECT set_config('gp.optimizer_enable_tablescan', 'off', true),
                      gp_orca.traceflags() AS f) t2;" "2"

is "and switching it back on takes them away again" \
   "SET gp.optimizer_enable_tablescan = on;
    SELECT count(*) FROM (SELECT gp_orca.traceflags()) t;" "1"

# A setting the mapping reads through the table rather than by hand.
is "a print setting turns its flag on" \
   "SELECT array_length(t2.f, 1) - array_length(t1.f, 1) FROM
      (SELECT gp_orca.traceflags() AS f) t1,
      LATERAL (SELECT set_config('gp.optimizer_print_plan', 'on', true),
                      gp_orca.traceflags() AS f) t2;" "1"

# A negated entry: the flag is set when the setting is OFF.  Getting the
# polarity of one of these backwards would be invisible without a test.
is "a negated setting sets its flag when it is switched off" \
   "SELECT array_length(t2.f, 1) - array_length(t1.f, 1) FROM
      (SELECT gp_orca.traceflags() AS f) t1,
      LATERAL (SELECT set_config('gp.optimizer_enable_outerjoin_rewrite', 'off', true),
                      gp_orca.traceflags() AS f) t2;" "1"

# The join-order heuristic is a set of rules rather than one flag, and each
# choice is a different set.
is "each join-order heuristic asks for a different set of rules" \
   "SELECT count(DISTINCT f) FROM (
      SELECT set_config('gp.optimizer_join_order', o, true),
             gp_orca.traceflags()::text AS f
        FROM unnest(ARRAY['query','greedy','exhaustive','exhaustive2']) o) t;" "4"

is "the cost model asks for a flag when it is not the calibrated one" \
   "SELECT array_length(t2.f, 1) - array_length(t1.f, 1) FROM
      (SELECT gp_orca.traceflags() AS f) t1,
      LATERAL (SELECT set_config('gp.optimizer_cost_model', 'legacy', true),
                      gp_orca.traceflags() AS f) t2;" "1"

# optimizer_minidump is the entry Cloudberry reads through "(bool *) &" past a
# FIXME of its own: it was a bool once and is an enum now, so that reads one
# byte of an int.  The port compares it as an enum, which is what makes this
# testable at all.
is "the minidump setting is read as the enum it is" \
   "SELECT array_length(t2.f, 1) - array_length(t1.f, 1) FROM
      (SELECT gp_orca.traceflags() AS f) t1,
      LATERAL (SELECT set_config('gp.optimizer_minidump', 'always', true),
                      gp_orca.traceflags() AS f) t2;" "1"

is "and asks for nothing when it is left on failures only" \
   "SELECT array_length(t2.f, 1) - array_length(t1.f, 1) FROM
      (SELECT gp_orca.traceflags() AS f) t1,
      LATERAL (SELECT set_config('gp.optimizer_minidump', 'onerror', true),
                      gp_orca.traceflags() AS f) t2;" "0"

# The disabled-xform array is sized from ORCA's own sentinel rather than a
# constant, because the id space grows with every rule added upstream.
is "no flag is asked for twice" \
   "SELECT count(*) FROM (
      SELECT unnest(gp_orca.traceflags()) f GROUP BY 1 HAVING count(*) > 1) d;" "0"

is "and the flags come back sorted, which is how a bit set reads out" \
   "SELECT gp_orca.traceflags() = (SELECT array_agg(f ORDER BY f)
                                     FROM unnest(gp_orca.traceflags()) f);" "t"

###############################################################################
echo
echo "18. the gpdb:: wrapper layer, which nothing else can call yet"
###############################################################################
# gpdbwrappers.cpp is what the translator calls to reach the server: 198
# functions, each one a place where a PostgreSQL longjmp becomes a GPOS
# exception.  It has no caller until the translator exists, so these probes
# reach it from SQL instead -- otherwise the whole layer would go untested
# through the largest part of the port, at the moment it is most likely to be
# wrong.
#
# What is probed is what the port had to change, not what Cloudberry wrote.

# --- the eleven operator OIDs the port had to name itself --------------------
#
# PostgreSQL has these operators and gives none of them a C name; Cloudberry
# adds an oid_symbol to pg_operator.dat, which the port cannot do without
# patching a catalog.  So compat/cb_operator_oids.h names them, and a wrong
# number there would compile and would quietly tell ORCA that a lossy
# operator preserves distinct values.
#
# The test resolves each operator by signature rather than by OID, so it
# compares the header against the catalog and not against itself.
is "text || text preserves distinct values" \
   "SELECT gp_orca.ndv_preserving('||(text,text)'::regoperator::oid);" "t"

is "int4 + int4 does" \
   "SELECT gp_orca.ndv_preserving('+(int4,int4)'::regoperator::oid);" "t"

is "int8 + int8 does" \
   "SELECT gp_orca.ndv_preserving('+(int8,int8)'::regoperator::oid);" "t"

is "numeric + numeric does" \
   "SELECT gp_orca.ndv_preserving('+(numeric,numeric)'::regoperator::oid);" "t"

is "date + interval does" \
   "SELECT gp_orca.ndv_preserving('+(date,interval)'::regoperator::oid);" "t"

is "date + int4 does" \
   "SELECT gp_orca.ndv_preserving('+(date,int4)'::regoperator::oid);" "t"

is "int4 + date does" \
   "SELECT gp_orca.ndv_preserving('+(int4,date)'::regoperator::oid);" "t"

is "date + time does" \
   "SELECT gp_orca.ndv_preserving('+(date,time)'::regoperator::oid);" "t"

is "date + timetz does" \
   "SELECT gp_orca.ndv_preserving('+(date,timetz)'::regoperator::oid);" "t"

is "timestamp + interval does" \
   "SELECT gp_orca.ndv_preserving('+(timestamp,interval)'::regoperator::oid);" "t"

is "interval + timestamp does" \
   "SELECT gp_orca.ndv_preserving('+(interval,timestamp)'::regoperator::oid);" "t"

# All eleven at once, which is the test that would catch one of them having
# been given the number of a different operator.
is "and that is exactly eleven operators, no more" \
   "SELECT count(*) FROM (
      SELECT oid FROM pg_operator WHERE gp_orca.ndv_preserving(oid)) t;" "11"

# The ones that are not.  Multiplication changes the number of distinct
# values when one side repeats; subtraction of two columns does too.
is "int4 - int4 does not preserve distinct values" \
   "SELECT gp_orca.ndv_preserving('-(int4,int4)'::regoperator::oid);" "f"

is "int4 * int4 does not" \
   "SELECT gp_orca.ndv_preserving('*(int4,int4)'::regoperator::oid);" "f"

is "and neither does equality" \
   "SELECT gp_orca.ndv_preserving('=(int4,int4)'::regoperator::oid);" "f"

# --- two access-method functions PostgreSQL renamed or made const ------------
q "CREATE TABLE wrap_heap(a int, b text);" > /dev/null

is "a heap table is stored with the heap access method" \
   "SELECT gp_orca.rel_am_name('wrap_heap'::regclass);" "heap"

is "btree's handler resolves to an IndexAmRoutine" \
   "SELECT gp_orca.index_am_resolves(
      (SELECT amhandler FROM pg_am WHERE amname = 'btree'));" "t"

is "and so does hash's" \
   "SELECT gp_orca.index_am_resolves(
      (SELECT amhandler FROM pg_am WHERE amname = 'hash'));" "t"

# A table access method handler is not an index one, and PostgreSQL says so
# rather than returning something unusable.
is "a table access method handler is refused, not mistaken for an index one" \
   "SELECT gp_orca.index_am_resolves(
      (SELECT amhandler FROM pg_am WHERE amname = 'heap')) IS NULL;" "t"

# --- the one that would have been wrong --------------------------------------
#
# Cloudberry calls a three-argument statext_dependencies_load() whose third
# argument means "return nothing rather than raising when the dependencies
# are not built".  PostgreSQL's takes two arguments and raises.  ORCA asks
# this of every extended statistics object it meets, so "not built" is the
# ordinary case, and a port that simply dropped the argument would fail to
# plan any query over a table carrying a statistics object of another kind.
q "CREATE TABLE wrap_stats(a int, b int, c int);
   INSERT INTO wrap_stats SELECT i, i % 10, i % 100 FROM generate_series(1, 200) i;
   CREATE STATISTICS wrap_deps (dependencies) ON a, b FROM wrap_stats;
   CREATE STATISTICS wrap_ndist (ndistinct) ON a, c FROM wrap_stats;
   ANALYZE wrap_stats;" > /dev/null

is "a statistics object with dependencies built reports them" \
   "SELECT gp_orca.mv_dependencies(
      (SELECT oid FROM pg_statistic_ext WHERE stxname = 'wrap_deps'));" "t"

# The point of the whole change: this one has no dependencies, and asking
# must be an answer rather than an error.
is "one built for another kind answers no, and does not raise" \
   "SELECT gp_orca.mv_dependencies(
      (SELECT oid FROM pg_statistic_ext WHERE stxname = 'wrap_ndist'));" "f"

# And before ANALYZE there is no data row at all, which is the same question
# asked one step earlier.
q "CREATE TABLE wrap_fresh(a int, b int);
   CREATE STATISTICS wrap_unbuilt (dependencies) ON a, b FROM wrap_fresh;" > /dev/null

is "and one never analyzed answers no as well" \
   "SELECT gp_orca.mv_dependencies(
      (SELECT oid FROM pg_statistic_ext WHERE stxname = 'wrap_unbuilt'));" "f"

# --- the syscache callback whose signature changed ---------------------------
#
# PostgreSQL 19 types the callback's cache id as SysCacheIdentifier rather
# than int.  In C that is the same argument; in C++ it is a different one, so
# a port that only silenced the compile error would register nothing and the
# metadata cache would never be told the catalog had changed.
# Both calls have to be in one session, because the counter is per-backend
# and each q() opens its own.  The first call registers the callbacks and
# always answers true; the second has nothing to report.
is "two calls in one backend: the first wants a reset, the second does not" \
   "SELECT gp_orca.mdcache_needs_reset();
    SELECT gp_orca.mdcache_needs_reset();" "t
f"

# And a catalog change between them is noticed -- which, given the line
# above, is what proves the callbacks were registered with something
# PostgreSQL actually calls, rather than merely accepted.
is "and a catalog change between them is noticed" \
   "SELECT gp_orca.mdcache_needs_reset();
    CREATE TABLE mdc_probe(a int);
    SELECT gp_orca.mdcache_needs_reset();" "t
t"

# --- the wrapper that gained an answer with the "gp" label -------------------
#
# ORCA's relcache translator asks every relation how its rows are spread, so
# this wrapper had to work at M1.  A relation with no policy is ordinary here
# and impossible in Cloudberry, whose DDL gives every table one.
is "a table with no gp label has no distribution policy" \
   "SELECT gp_orca.wrapper_policy('wrap_heap'::regclass) IS NULL;" "t"

q "SECURITY LABEL FOR gp ON TABLE wrap_heap IS 'distributed_by=(\"a\")';" > /dev/null

is "and one labelled with a key is hash-partitioned on it" \
   "SELECT gp_orca.wrapper_policy('wrap_heap'::regclass);" "partitioned"

q "SECURITY LABEL FOR gp ON TABLE wrap_heap IS 'distributed_by=random';" > /dev/null

is "a randomly distributed one says so" \
   "SELECT gp_orca.wrapper_policy('wrap_heap'::regclass);" "random"

q "SECURITY LABEL FOR gp ON TABLE wrap_heap IS 'distributed_by=replicated';" > /dev/null

is "and a replicated one says so" \
   "SELECT gp_orca.wrapper_policy('wrap_heap'::regclass);" "replicated"

# The wrapper reads the same label gp_orca.relation_policy() does, through a
# different path: one is the compat layer in C, the other the wrapper in C++.
# They must not disagree.
is "the wrapper and the compat layer read the same label" \
   "SELECT gp_orca.wrapper_policy('wrap_heap'::regclass)
           = gp_orca.relation_policy('wrap_heap'::regclass);" "t"

# --- the fallback path, made visible -----------------------------------------
#
# Fifteen wrappers belong to M2 or M5 and raise rather than returning a
# plausible answer, because a plan built on a false premise is the one
# outcome the fallback design exists to prevent.  This calls one and reads
# the exception back: ExmaDXL is 200, and the minor is the unsupported-feature
# code.
has "a wrapper that is not ported yet raises rather than answering" \
   "SELECT gp_orca.unported_raise();" "200/"

is "and it does not take the backend down with it" \
   "SELECT 1;" "1"

is "nor leave the session unable to plan" \
   "SELECT count(*) FROM wrap_stats;" "200"

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
