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
	# gp_matview for section 23: it maintains an incremental view with
	# queries it hands the planner, which ORCA plans.
	echo "shared_preload_libraries = 'gp_core,gp_orca,gp_matview'"
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

# The walker has one case for thirty-odd expression nodes -- function calls,
# aggregates, IS NULL -- which reads their collation.  A port that lost that
# case's body fell through to the one below it, which refuses outright, and
# every query with a function call or an aggregate in it read as a
# non-default collation.  Running the translator found it.
is "a function call and an aggregate over the default collation do not count" \
   "SELECT non_default_collation FROM gp_orca.explain_refusal(
      'SELECT upper(t), count(*) FROM pts WHERE t IS NOT NULL GROUP BY 1');" "f"

is "and a function call over COLLATE \"C\" still does" \
   "SELECT non_default_collation FROM gp_orca.explain_refusal(
      'SELECT upper(u) FROM pts');" "t"

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
# everything works would not answer that.  Until the translator's first group
# of operators the reason was always the same one, that there was no
# translator; now a query is planned, or declined with ORCA's reason, and
# what is tested is that the counters tell the two apart.

q "SELECT gp_orca.reset_fallbacks();" > /dev/null

is "every reason is listed, plus the plans ORCA made" \
   "SELECT count(*) > 1 FROM gp_orca.fallbacks();" "t"

is "and each one says what it means" \
   "SELECT count(*) FROM gp_orca.fallbacks() WHERE means IS NULL OR means = '';" "0"

is "nothing is counted twice" \
   "SELECT count(*) FROM (SELECT reason FROM gp_orca.fallbacks()
                           GROUP BY reason HAVING count(*) > 1) d;" "0"

is "and there is no reason left that says the translator is missing" \
   "SELECT count(*) FROM gp_orca.fallbacks() WHERE reason = 'no_translator';" "0"

# The hook is installed, so a statement that reaches the planner is counted.
# Reading the counter is itself a statement, which is why each check is that
# a counter moved rather than that it holds a particular number.
q "SELECT gp_orca.reset_fallbacks();" > /dev/null
before=$(q "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'planned';")
q "SELECT 1 FROM es WHERE a = 1;" > /dev/null
after=$(q "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'planned';")
[ "$after" -gt "$before" ] \
	&& ok "a query ORCA plans is counted as planned" \
	|| notok "a query ORCA plans is counted as planned" \
	         "before [$before], after [$after]"

# A row lock is a query ORCA looks at and declines (see "FOR UPDATE, which
# would lock nothing" in section 21): counted, with that reason, and planned
# by PostgreSQL instead.  Until T1 this was a join, which ORCA now plans.
before=$(q "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'declined';")
q "SELECT count(*) FROM (SELECT e1.a FROM es e1 JOIN es e2 USING (a) FOR UPDATE OF e1) s;" > /dev/null
after=$(q "SELECT count FROM gp_orca.fallbacks() WHERE reason = 'declined';")
[ "$after" -gt "$before" ] \
	&& ok "a query ORCA will not plan yet is counted as declined" \
	|| notok "a query ORCA will not plan yet is counted as declined" \
	         "before [$before], after [$after]"

# gp.optimizer off is a different reason from "would not", and telling them
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
"$PSQL" -X -q -t -A -d postgres -c "SELECT count(*) FROM (SELECT e1.a FROM es e1 JOIN es e2 USING (a) FOR UPDATE OF e1) s;" > /dev/null 2>&1
is "and another backend's fallbacks are visible from this one" \
   "SELECT count > 0 FROM gp_orca.fallbacks() WHERE reason = 'declined';" "t"

# Resetting is restricted, because one session doing it loses everybody
# else's numbers.
is "resetting is not something every user may do" \
   "SELECT has_function_privilege('public', 'gp_orca.reset_fallbacks()', 'execute');" "f"

# The plan still comes out when ORCA declines, and it is PostgreSQL's.  A hook
# that counted and then lost the plan would pass every test above.
is "a declined query still gets a plan, and it runs" \
   "SELECT count(*) FROM (SELECT e1.a FROM es e1 JOIN es e2 USING (a) WHERE e1.a = 1 FOR UPDATE OF e1) s;" "10000"

has "and EXPLAIN says whose plan it is" \
    "EXPLAIN (COSTS OFF) SELECT count(*) FROM (SELECT e1.a FROM es e1 JOIN es e2 USING (a) FOR UPDATE OF e1) s;" \
    "Optimizer: Postgres query optimizer"

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

# --- the settings that are not trace flags -----------------------------------
#
# "ORCA's settings" is two surfaces and not one, which is not obvious and was
# not in the plan.  Everything above becomes a bit in the set the optimizer is
# handed.  These do not: they configure the optimizer's context -- the cost
# model's factors, the search strategy, the size of the metadata cache, the
# thresholds at which a transform stops being tried -- and COptTasks reads
# them once per query when it builds a COptimizerConfig.  CConfigParamMapping
# never sees one.
#
# They arrive now because the translator cannot be sized without them: they are
# 33 of the names it reads.

is "the context settings are there too" \
   "SELECT count(*) FROM pg_settings WHERE name IN (
      'gp.optimizer_mdcache_size', 'gp.optimizer_segments',
      'gp.optimizer_nestloop_factor', 'gp.optimizer_damping_factor_join',
      'gp.optimizer_search_strategy_path', 'gp.optimizer_use_gpdb_allocators');" "6"

is "and carry Cloudberry's defaults" \
   "SELECT string_agg(name || '=' || setting, ' ' ORDER BY name)
      FROM pg_settings WHERE name IN (
        'gp.optimizer_mdcache_size', 'gp.optimizer_join_order_threshold',
        'gp.optimizer_nestloop_factor');" \
   "gp.optimizer_join_order_threshold=10 gp.optimizer_mdcache_size=16384 gp.optimizer_nestloop_factor=1024"

is "and Cloudberry's bounds, which are narrower than an int for two of them" \
   "SELECT string_agg(name || ' ' || min_val || '..' || max_val, ', ' ORDER BY name)
      FROM pg_settings WHERE name IN (
        'gp.optimizer_join_order_threshold', 'gp.optimizer_skew_factor');" \
   "gp.optimizer_join_order_threshold 0..12, gp.optimizer_skew_factor 0..100"

refused "so a value outside them is refused" \
        "SET gp.optimizer_join_order_threshold = 13;" \
        "outside the valid range"

is "the damping factors are reals with a 0..1 range" \
   "SELECT vartype || ' ' || min_val || '..' || max_val FROM pg_settings
     WHERE name = 'gp.optimizer_damping_factor_groupby';" "real 0..1"

is "the search strategy is a string, empty for ORCA's own defaults" \
   "SELECT vartype || '[' || setting || ']' FROM pg_settings
     WHERE name = 'gp.optimizer_search_strategy_path';" "string[]"

# The allocator is chosen when ORCA's memory pool manager is built and every
# pool made afterwards inherits the choice, so a session may not change it.
# Cloudberry makes it PGC_POSTMASTER for the same reason.
is "the allocator setting is postmaster context, as in Cloudberry" \
   "SELECT context FROM pg_settings WHERE name = 'gp.optimizer_use_gpdb_allocators';" \
   "postmaster"

refused "and a session cannot change it" \
        "SET gp.optimizer_use_gpdb_allocators = off;" \
        "cannot be changed"

# This is the assertion that the split is real.  If one of these ever became a
# trace flag by accident -- a row added to the wrong list -- the flag set would
# move when it was set, and nothing else would notice.
is "and none of them moves the flag set, because none is a flag" \
   "SELECT count(DISTINCT f) FROM (
      SELECT gp_orca.traceflags()::text AS f
      UNION ALL
      SELECT (SELECT gp_orca.traceflags()::text
                FROM (SELECT set_config(n, v, true)) s)
        FROM (VALUES ('gp.optimizer_mdcache_size', '4096'),
                     ('gp.optimizer_segments', '7'),
                     ('gp.optimizer_nestloop_factor', '2048'),
                     ('gp.optimizer_damping_factor_join', '0.5'),
                     ('gp.optimizer_skew_factor', '50'),
                     ('gp.optimizer_xform_bind_threshold', '100'),
                     ('gp.optimizer_plan_id', '3'),
                     ('gp.optimizer_metadata_caching', 'off'),
                     ('gp.optimizer_enable_dml', 'off'),
                     ('gp.optimizer_enable_foreign_table', 'off')) v(n, v)) t;" "1"

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

# A table access method handler is not an index one, and PostgreSQL raises
# rather than returning something unusable.  This is the whole error path in
# one line: a PostgreSQL elog becomes a GPOS exception inside GP_WRAP, the
# probe catches it, and the caller turns it back into a PostgreSQL error.
#
# The caller has to raise.  Returning a value would leave the backend with a
# transaction PostgreSQL never aborted, and this test took the server down
# when it expected NULL here.
refused "a table access method handler is refused, not mistaken for an index one" \
   "SELECT gp_orca.index_am_resolves(
      (SELECT amhandler FROM pg_am WHERE amname = 'heap'));" \
   "not an index access method handler"

is "and the session is still usable afterwards" \
   "SELECT gp_orca.index_am_resolves(
      (SELECT amhandler FROM pg_am WHERE amname = 'btree'));" "t"

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

# Before ANALYZE there is no pg_statistic_ext_data row at all, and that is a
# different question: allow_null covers "the row is there and this kind of
# statistic is null", not "there is no row".  Cloudberry raises here too, so
# the port matching it is the right answer rather than a gap.
#
# ORCA never asks it.  GetRelationExtStatistics returns nothing for an object
# with no data row and reports only the kinds that are built, so the objects
# it goes on to ask about always have both.
q "CREATE TABLE wrap_fresh(a int, b int);
   CREATE STATISTICS wrap_unbuilt (dependencies) ON a, b FROM wrap_fresh;" > /dev/null

refused "one never analyzed at all raises, as it does on Cloudberry" \
   "SELECT gp_orca.mv_dependencies(
      (SELECT oid FROM pg_statistic_ext WHERE stxname = 'wrap_unbuilt'));" \
   "the optimizer raised asking for functional dependencies"

is "and the compat layer never offers ORCA such an object" \
   "SELECT count(*) FROM gp_orca.ext_stats('wrap_fresh'::regclass);" "0"

# --- the syscache callback whose signature changed ---------------------------
#
# PostgreSQL 19 types the callback's cache id as SysCacheIdentifier rather
# than int.  In C that is the same argument; in C++ it is a different one, so
# a port that only silenced the compile error would register nothing and the
# metadata cache would never be told the catalog had changed.
# Both calls have to be in one session, because the counter is per-backend
# and each q() opens its own.  The first call registers the callbacks and
# answers no: Cloudberry compares the counter with the last one it saw, and
# on the first call both are zero.  So a backend that has never planned
# anything is told its metadata cache is fresh, which it is.
is "two calls in one backend, with nothing between them, want no reset" \
   "SELECT gp_orca.mdcache_needs_reset();
    SELECT gp_orca.mdcache_needs_reset();" "f
f"

# And a catalog change between them is noticed -- which, given the line
# above, is what proves the callbacks were registered with something
# PostgreSQL actually calls, rather than merely accepted.
#
# With gp.optimizer off, because ORCA asks the same question before it plans
# each statement -- that is what the answer is for -- and the probe's own
# SELECT is a statement ORCA would be asked about, which would take the
# answer before the probe ran.
is "and a catalog change between them is noticed" \
   "SET gp.optimizer = off;
    SELECT gp_orca.mdcache_needs_reset();
    CREATE TABLE mdc_probe(a int);
    SELECT gp_orca.mdcache_needs_reset();" "f
t"

is "and with ORCA asked in between, ORCA is the one told" \
   "SELECT gp_orca.mdcache_needs_reset();
    CREATE TABLE mdc_probe2(a int);
    SELECT count(*) FROM mdc_probe2;
    SET gp.optimizer = off;
    SELECT gp_orca.mdcache_needs_reset();" "f
0
f"

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
   "SELECT (gp_orca.relation_policy('wrap_heap'::regclass)).kind || '/' ||
           gp_orca.wrapper_policy('wrap_heap'::regclass);" "replicated/replicated"

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

###############################################################################
echo
echo "19. sharing an aggregate's transition state, against the planner"
###############################################################################
# find_compatible_agg() and find_compatible_trans() decide which aggregates in
# a query may share an Agg node's transition state.  They are the last two
# functions of the compat layer with no caller -- CTranslatorDXLToPlStmt is the
# only one there will be -- and a wrong answer from either is a wrong plan
# rather than a failure: share too much and one aggregate's transition state
# is read as another's.
#
# They need no fixture.  PostgreSQL records its own answer on the node:
# Aggref.aggno and Aggref.aggtransno are -1 after parsing and are set by
# preprocess_aggref().  So gp_orca.agg_sharing() plans the query, then replays
# the port's matchers over the very same Aggrefs, and the two answers sit side
# by side.  Any difference is a defect in the port.

q "CREATE TABLE aggt(a int, b int8, c numeric, d text);
   INSERT INTO aggt SELECT i, i, i, i::text FROM generate_series(1, 50) i;" > /dev/null

# Every row must agree.  $3, when given, is the planner's shape as
# aggno/transno per aggregate -- asserted as well, so that each case says
# which sharing it is exercising rather than only that the two agree.
aggs() {
	local dis shape
	dis=$(q "SELECT count(*) FROM gp_orca.agg_sharing('$2')
	          WHERE planner_aggno IS DISTINCT FROM compat_aggno
	             OR planner_transno IS DISTINCT FROM compat_transno;")
	shape=$(q "SELECT string_agg(planner_aggno || '/' || planner_transno, ',' ORDER BY n)
	             FROM gp_orca.agg_sharing('$2');")
	if [ "$dis" != "0" ]; then
		notok "$1" "the port disagreed with the planner on $dis of them (shape $shape)"
	elif [ -n "${3:-}" ] && [ "$shape" != "$3" ]; then
		notok "$1" "agreed, but on [$shape] where [$3] was expected"
	else
		ok "$1"
	fi
}

# --- the two ends of the range ----------------------------------------------
aggs "the same call twice is one aggregate" \
     "SELECT count(a), count(a) FROM aggt" "0/0,0/0"

aggs "two different aggregates share nothing" \
     "SELECT count(a), sum(b) FROM aggt" "0/0,1/1"

# --- the case the second function exists for ---------------------------------
#
# sum(numeric) and avg(numeric) are different aggregates with different final
# functions, and both accumulate with numeric_avg_accum.  find_compatible_agg
# finds no exact match and reports the transno as a candidate;
# find_compatible_trans then matches on the transition function.  Two
# aggregates, one transition state.
aggs "sum and avg over numeric share one transition state" \
     "SELECT sum(c), avg(c) FROM aggt" "0/0,1/0"

aggs "and so do var_samp and stddev_samp" \
     "SELECT var_samp(c), stddev_samp(c) FROM aggt" "0/0,1/0"

aggs "three aggregates, two of which share" \
     "SELECT sum(c), avg(c), count(c) FROM aggt" "0/0,1/0,2/1"

# --- same inputs, but nothing to share ---------------------------------------
aggs "max and min take the same input and cannot share" \
     "SELECT max(a), min(a) FROM aggt" "0/0,1/1"

# --- what makes two calls different ------------------------------------------
aggs "DISTINCT makes it a different aggregate" \
     "SELECT count(a), count(DISTINCT a) FROM aggt" "0/0,1/1"

aggs "so does a FILTER" \
     "SELECT count(a) FILTER (WHERE b > 0), count(a) FROM aggt" "0/0,1/1"

aggs "so does counting rows rather than a column" \
     "SELECT count(a), count(*) FROM aggt" "0/0,1/1"

aggs "and so does the argument type" \
     "SELECT sum(a), sum(b) FROM aggt" "0/0,1/1"

# --- a volatile argument stops all of it -------------------------------------
#
# find_compatible_agg refuses to reuse an aggregate whose arguments contain a
# volatile function, because the second call would not compute the same thing.
aggs "a volatile argument is never shared, even with itself" \
     "SELECT count(random()), count(random()) FROM aggt" "0/0,1/1"

# --- more than one place aggregates are found --------------------------------
#
# subquery_planner walks the target list and then HAVING, and so does the
# probe.  An aggregate written in both places is one aggregate.
aggs "HAVING is walked after the target list" \
     "SELECT a, count(b) FROM aggt GROUP BY a HAVING count(b) > 0" "0/0,0/0"

aggs "and an aggregate only in HAVING is found" \
     "SELECT a FROM aggt GROUP BY a HAVING sum(b) > 0" "0/0"

# --- ordering and direct arguments -------------------------------------------
aggs "an ordered-set aggregate" \
     "SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY c) FROM aggt" "0/0"

aggs "the same one twice is still one" \
     "SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY c),
             percentile_cont(0.5) WITHIN GROUP (ORDER BY c) FROM aggt" "0/0,0/0"

# A different percentile is a different aggregate that shares the state, and
# this is the sharing rule at its sharpest.  find_compatible_agg compares the
# aggregated arguments, ORDER BY, DISTINCT, FILTER, collation and transition
# type -- but it compares the *direct* arguments only when deciding whether
# the call is identical.  So 0.5 and 0.9 are not the same aggregate, and they
# do accumulate the same rows the same way: two final functions over one
# transition state.
aggs "a different percentile is a different aggregate over the same state" \
     "SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY c),
             percentile_cont(0.9) WITHIN GROUP (ORDER BY c) FROM aggt" "0/0,1/0"

# --- a polymorphic transition type -------------------------------------------
#
# array_agg's declared transition type is internal and its real one is
# resolved from the input.  GetAggregateInfo resolves it and hands it back
# without writing it into the Aggref, so the caller has to -- and
# find_compatible_agg compares that field.  A translator that left the line
# out would find every polymorphic aggregate compatible with every other.
aggs "array_agg over two different types is two aggregates" \
     "SELECT array_agg(a), array_agg(d) FROM aggt" "0/0,1/1"

aggs "and over the same type, written twice, is one" \
     "SELECT array_agg(a), array_agg(a) FROM aggt" "0/0,0/0"

# --- the probe's own edges ---------------------------------------------------
refused "a query with no aggregates is refused rather than reported empty" \
   "SELECT count(*) FROM gp_orca.agg_sharing('SELECT 1');" \
   "no aggregates to compare"

refused "and so is more than one statement" \
   "SELECT count(*) FROM gp_orca.agg_sharing('SELECT count(1); SELECT count(2);');" \
   "one statement"

###############################################################################
echo
echo "20. ORCA's metadata, from the relcache translator"
###############################################################################
# The first of the translator proper.  gp_orca.md_dxl() asks ORCA's metadata
# accessor for an object the way the optimizer will -- through a
# CMDProviderRelcache, with the metadata cache in front -- and returns the DXL
# it gets back.  Until Query to DXL exists to consume the relcache translator,
# this is the only thing that can; afterwards it is still the answer to "what
# does ORCA think this table is".
#
# Each assertion is on something the port changed or found by running this,
# not on something Cloudberry wrote.

q "CREATE TABLE md_plain (a int, b text);
   CREATE INDEX md_plain_a ON md_plain (a);
   INSERT INTO md_plain SELECT i % 7, 'x' FROM generate_series(1, 1000) i;
   ANALYZE md_plain;
   CREATE TABLE md_keyed (id int PRIMARY KEY, v int CHECK (v > 0));
   CREATE TABLE md_parts (k int, v int) PARTITION BY RANGE (k);
   CREATE TABLE md_parts_1 PARTITION OF md_parts FOR VALUES FROM (0) TO (10);
   CREATE TABLE md_parent (a int);
   CREATE TABLE md_child () INHERITS (md_parent);
   CREATE TABLE md_ml (k int, j int) PARTITION BY RANGE (k);
   CREATE TABLE md_ml_1 PARTITION OF md_ml FOR VALUES FROM (0) TO (10) PARTITION BY RANGE (j);
   CREATE FOREIGN DATA WRAPPER md_nofdw;
   CREATE SERVER md_nosrv FOREIGN DATA WRAPPER md_nofdw;
   CREATE FOREIGN TABLE md_ft (a int) SERVER md_nosrv;" > /dev/null

# --- the first things running it found ---------------------------------------
#
# Cloudberry changed RelationGetPartitionDesc's contract without changing its
# signature: PostgreSQL asserts the relation is partitioned, Cloudberry returns
# NULL when it is not, and the translator asks it of every relation as "is this
# partitioned?".  The first plain table failed the assertion.
has "a plain table is described, which asks the partition question of a table that has none" \
    "SELECT gp_orca.md_dxl('relation', 'md_plain'::regclass);" 'Name="md_plain"'

# InitDXL() is ORCA's fourth initialisation step, which Cloudberry takes in
# COptTasks rather than in InitGPOPT.  Without it the first serialization
# failed on "Token map not initialized yet".
has "and serialized, which needs the DXL token map" \
    "SELECT gp_orca.md_dxl('relation', 'md_plain'::regclass);" '<dxl:Columns>'

# --- one node: every relation is where every row is ---------------------------
#
# COptTasks turns Motions off for a query that touches no distributed table,
# and that is what keeps a Motion out of every M1 plan -- not the one segment.
# So the relcache translator reports every relation as coordinator-only on one
# node, whatever its label records.  A DISTRIBUTED BY label would otherwise make
# ORCA plan a Gather Motion from a segment that does not exist.
is "a heap table reads as heap, on the coordinator" \
   "SELECT substring(gp_orca.md_dxl('relation', 'md_plain'::regclass)
                     from 'StorageType=\"[^\"]*\" DistributionPolicy=\"[^\"]*\"');" \
   'StorageType="Heap" DistributionPolicy="MasterOnly"'

is "a hash-distributed label is still recorded" \
   "SELECT kind FROM gp_orca.relation_policy('dist_hash'::regclass);" "hash"

is "and ORCA is told the table is on the coordinator all the same" \
   "SELECT substring(gp_orca.md_dxl('relation', 'dist_hash'::regclass)
                     from 'DistributionPolicy=\"[^\"]*\"');" 'DistributionPolicy="MasterOnly"'

is "so is a replicated one, and a random one" \
   "SELECT string_agg(substring(gp_orca.md_dxl('relation', t::regclass)
                                from 'DistributionPolicy=\"[^\"]*\"'), ' ')
      FROM unnest(ARRAY['dist_repl', 'dist_rand']) t;" \
   'DistributionPolicy="MasterOnly" DistributionPolicy="MasterOnly"'

# --- no gp_segment_id ---------------------------------------------------------
#
# Cloudberry's system attributes run to -8, and gp_segment_id is -7; PostgreSQL
# 19's stop at tableoid, -6.  AddSystemColumns loops down to
# FirstLowInvalidHeapAttributeNumber, so it is right as written; the key sets
# were not.
is "the system columns stop at tableoid" \
   "SELECT string_agg(m[1], ',' ORDER BY m[1]::int DESC)
      FROM regexp_matches(gp_orca.md_dxl('relation', 'md_plain'::regclass),
                          'Attno=\"(-[0-9]+)\"', 'g') m;" "-1,-2,-3,-4,-5,-6"

# The default key is {ctid}: Cloudberry's is {gp_segment_id, ctid}, because a
# ctid is unique only within one segment's copy of a table.  On one node every
# row is in one place.  Keys are positions in the column list, which for
# md_plain (a, b) puts ctid at 2.
is "a table with no key of its own is keyed on ctid alone" \
   "SELECT substring(gp_orca.md_dxl('relation', 'md_plain'::regclass) from 'Keys=\"[^\"]*\"');" \
   'Keys="2"'

is "a primary key comes first, and the default key after it" \
   "SELECT substring(gp_orca.md_dxl('relation', 'md_keyed'::regclass) from 'Keys=\"[^\"]*\"');" \
   'Keys="0;2"'

is "a partitioned table is keyed on tableoid and ctid" \
   "SELECT substring(gp_orca.md_dxl('relation', 'md_parts'::regclass) from 'Keys=\"[^\"]*\"');" \
   'Keys="7,2"'

# --- access methods that are extension objects in the port --------------------
#
# AO, AOCO, PAX and bitmap have fixed OIDs in Cloudberry and none here, so they
# are found by name.  A partitioned table's relam is 0, and so is the answer to
# looking up an access method that is not installed; compared bare, every
# partitioned table would have been PAX.
is "a partitioned table has no access method of its own" \
   "SELECT relam FROM pg_class WHERE oid = 'md_parts'::regclass;" "0"

is "and reads as the storage of its leaves, not as PAX" \
   "SELECT substring(gp_orca.md_dxl('relation', 'md_parts'::regclass) from 'StorageType=\"[^\"]*\"');" \
   'StorageType="Heap"'

# A foreign table runs where PostgreSQL runs it, on the coordinator: Cloudberry
# reads ForeignTable.exec_location, a field PostgreSQL 19 does not have.
is "a foreign table reads as foreign, on the coordinator" \
   "SELECT substring(gp_orca.md_dxl('relation', 'md_ft'::regclass)
                     from 'StorageType=\"[^\"]*\" DistributionPolicy=\"[^\"]*\"');" \
   'StorageType="Foreign" DistributionPolicy="MasterOnly"'

# --- the other kinds of object ------------------------------------------------
has "an index is described" \
    "SELECT gp_orca.md_dxl('index', 'md_plain_a'::regclass);" 'IndexType="B-tree"'

# The scalar translator's stand-alone path: a constraint's expression, turned
# into DXL.  A subquery is the one thing it refuses there, and PostgreSQL does
# not allow one in a CHECK constraint.
has "a check constraint comes back as its expression" \
    "SELECT gp_orca.md_dxl('check_constraint',
              (SELECT oid FROM pg_constraint
                WHERE conrelid = 'md_keyed'::regclass AND contype = 'c'));" \
    'ComparisonOperator="&gt;"'

has "a type is described" \
    "SELECT gp_orca.md_dxl('type', 'int4'::regtype);" 'Name="int4"'

# get_compatible_hash_opfamily was deferred to M2 because its name says it asks
# whether two families hash compatibly.  Its body is a pg_amop search, and ORCA
# asks it of every operator; deferred, it made ORCA refuse every operator.  The
# answer is compared with the catalog, not with a number.
is "an equality operator names the hash family it is equality in" \
   "SELECT substring(gp_orca.md_dxl('operator', '=(int4,int4)'::regoperator)
                     from 'HashOpfamily Mdid=\"0\\.([0-9]+)\\.')::oid
         = (SELECT f.oid FROM pg_opfamily f JOIN pg_am a ON a.oid = f.opfmethod
             WHERE f.opfname = 'integer_ops' AND a.amname = 'hash');" "t"

# Cloudberry's legacy cdbhash opclasses are built-ins with fixed OIDs that
# neither PostgreSQL 19 nor any module of the port installs, so no operator
# belongs to one.  That is the true answer, not a stand-in for one.
is "and no legacy hash family, because there are none" \
   "SELECT position('LegacyHashOpfamily' IN
                    gp_orca.md_dxl('operator', '=(int4,int4)'::regoperator));" "0"

has "a function is described" \
    "SELECT gp_orca.md_dxl('function', 'lower(text)'::regprocedure);" 'Name="lower"'

has "an aggregate is described" \
    "SELECT gp_orca.md_dxl('aggregate', 'count(\"any\")'::regprocedure);" 'Name="count"'

# --- statistics ---------------------------------------------------------------
has "a table's row count comes from ANALYZE" \
    "SELECT gp_orca.md_dxl('relation_stats', 'md_plain'::regclass);" 'Rows="1000.000000"'

# MCVs become ORCA datums through the scalar translator, and that needs an
# optimizer context: without one the first column_stats request dereferenced a
# null context and took the backend down.
is "a column's seven most common values become seven buckets" \
   "SELECT count(*) FROM regexp_matches(
      gp_orca.md_dxl('column_stats', 'md_plain'::regclass, 1), '<dxl:StatsBucket', 'g');" "7"

has "and each bucket's bound is an ORCA datum of the column's type" \
    "SELECT gp_orca.md_dxl('column_stats', 'md_plain'::regclass, 1);" \
    '<dxl:LowerBound Closed="true" TypeMdid="0.23.1.0" Value="0"/>'

# --- the metadata cache -------------------------------------------------------
#
# The cache is reset when the catalog has changed since the last ask, which is
# what MDCacheNeedsReset's invalidation callbacks are for.  A stale cache here
# is a plan built for a table that no longer looks like that.
is "a column added between two asks is seen by the second" \
   "SELECT count(*) FROM regexp_matches(gp_orca.md_dxl('relation', 'md_keyed'::regclass),
                                       'Attno=\"[0-9]+\"', 'g');" "2"

is "after the ALTER" \
   "ALTER TABLE md_keyed ADD COLUMN extra int;
    SELECT count(*) FROM regexp_matches(gp_orca.md_dxl('relation', 'md_keyed'::regclass),
                                       'Attno=\"[0-9]+\"', 'g');" "3"

# --- what ORCA refuses, and that the reason arrives ---------------------------
refused "an inherited table is refused, and says so" \
        "SELECT gp_orca.md_dxl('relation', 'md_parent'::regclass);" \
        "does not support the following feature: Inherited tables"

refused "a multi-level partitioned table is refused, and says so" \
        "SELECT gp_orca.md_dxl('relation', 'md_ml'::regclass);" \
        "Multi-level partitioned tables"

refused "a relation that does not exist is a failed lookup" \
        "SELECT gp_orca.md_dxl('relation', 999999);" \
        "Lookup of object 6.999999.1.0 in cache failed"

refused "a zero OID never reaches ORCA, which would assert on it" \
        "SELECT gp_orca.md_dxl('relation', 0);" \
        "not an object ORCA can be asked about"

refused "an unknown kind is refused before ORCA is entered" \
        "SELECT gp_orca.md_dxl('frobnicate', 'md_plain'::regclass);" \
        "not a kind of metadata object"

refused "column statistics need a column" \
        "SELECT gp_orca.md_dxl('column_stats', 'md_plain'::regclass);" \
        "needs the column's attribute number"

# A PostgreSQL error inside ORCA: looking up a type that does not exist
# raises in the server, and GP_WRAP catches it before PostgreSQL's handler
# runs.  The original is still on the error stack, and it is re-thrown as it
# stands, as Cloudberry's CGPOptimizer does -- so PostgreSQL's own message
# arrives rather than "PG exception raised".
refused "a PostgreSQL error inside ORCA arrives as itself" \
        "SELECT gp_orca.md_dxl('type', 999999);" \
        "type with OID 999999 does not exist"

# Every refusal above was in a session of its own.  This is nine in one
# backend, each caught and rolled back to a savepoint, and then an answer from
# the same backend.  Three of the nine leave ORCA carrying a PostgreSQL error
# that PostgreSQL has not cleaned up; re-throwing it is what hands the cleanup
# back.  PostgreSQL allows five nested error levels, so a bridge that leaked
# one level per error would have stopped the backend well before the end.
is "one backend refuses nine times in a row and then answers" \
   "CREATE OR REPLACE FUNCTION md_refusals() RETURNS int LANGUAGE plpgsql AS \$\$
    DECLARE n int := 0;
    BEGIN
      FOR i IN 1..3 LOOP
        BEGIN PERFORM gp_orca.md_dxl('relation', 'md_parent'::regclass);
        EXCEPTION WHEN OTHERS THEN n := n + 1; END;
        BEGIN PERFORM gp_orca.md_dxl('relation', 999999);
        EXCEPTION WHEN OTHERS THEN n := n + 1; END;
        BEGIN PERFORM gp_orca.md_dxl('type', 999999);
        EXCEPTION WHEN OTHERS THEN n := n + 1; END;
      END LOOP;
      RETURN n;
    END \$\$;
    SELECT md_refusals()
           || ' ' || substring(gp_orca.md_dxl('relation', 'md_plain'::regclass)
                               from 'Name=\"[^\"]*\"');" '9 Name="md_plain"'

echo
echo "21. the queries of one table, planned by ORCA"

# T0 of the translator: Query to DXL, the optimizer's task and DXL to
# PlannedStmt together, for TableScan, Result, Limit, Sort, Agg, ValuesScan
# and Materialize.  DXL to PlannedStmt refuses every other operator, each
# body naming the group that brings it back, so a query either gets an ORCA
# plan made of those or falls back and says why.
#
# Most checks here are the same two ways: the query's answer under ORCA and
# under the planner, and that ORCA was the one that planned it -- an ORCA
# plan that returned the planner's rows because it quietly fell back would
# pass the first half alone.

q "CREATE TABLE t0 (a int, b text, c numeric, d date);
   INSERT INTO t0 SELECT i, 'v' || (i % 10), i * 1.5, date '2020-01-01' + i
     FROM generate_series(1, 1000) i;
   INSERT INTO t0 VALUES (NULL, NULL, NULL, NULL);
   ANALYZE t0;" > /dev/null

# q2 <setup> <query>: the query's output, after the setup, in one session.
q2() { "$PSQL" -X -q -t -A -d postgres -c "$1" -c "$2" 2>&1; }

# same <name> <query> [setup]: ORCA planned it, and answered as the planner does.
same() {
	local setup="${3:-SELECT}" orca pg plan
	plan=$(q2 "$setup" "EXPLAIN (COSTS OFF) $2")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tail -3 | tr '\n' '|')"; return ;;
	esac
	orca=$(q2 "$setup" "$2")
	pg=$(q2 "$setup; SET gp.optimizer = off" "$2")
	[ "$orca" = "$pg" ] && ok "$1" || notok "$1" "orca [$orca], planner [$pg]"
}

# declined <name> <query> <reason> [setup]: ORCA declined it, for that
# reason, and the planner's plan answers.
declined() {
	local setup="${4:-SELECT}" got pg
	got=$(q2 "$setup; SET gp.optimizer_trace_fallback = on" "$2")
	case "$got" in
		*"GPORCA failed to produce a plan"*"$3"*) ;;
		*) notok "$1" "expected a fallback for [$3], got [$(printf '%s' "$got" | head -3 | tr '\n' '|')]"; return ;;
	esac
	got=$(printf '%s\n' "$got" | grep -v 'GPORCA failed\|^DETAIL:')
	pg=$(q2 "$setup; SET gp.optimizer = off" "$2")
	[ "$got" = "$pg" ] && ok "$1" || notok "$1" "orca [$got], planner [$pg]"
}

# --- the operators ------------------------------------------------------------

same "a scan with a filter" \
     "SELECT * FROM t0 WHERE a < 3 ORDER BY a"

has "and EXPLAIN says ORCA made the plan, as Cloudberry's does" \
    "EXPLAIN (COSTS OFF) SELECT * FROM t0 WHERE a < 3" "Optimizer: GPORCA"

same "a sort and a limit" \
     "SELECT a, b FROM t0 ORDER BY a DESC NULLS LAST LIMIT 3"

same "an offset" \
     "SELECT a FROM t0 ORDER BY a LIMIT 3 OFFSET 2"

same "a hash aggregate" \
     "SELECT b, count(*), sum(c) FROM t0 GROUP BY b ORDER BY b"

same "a plain aggregate, several of them sharing one scan" \
     "SELECT count(*), min(a), max(a), avg(c), sum(c) FROM t0"

same "values" \
     "SELECT * FROM (VALUES (1, 'x'), (2, 'y'), (NULL, 'z')) v(i, s) ORDER BY 1"

same "a query that reads no table" \
     "SELECT 1 + 1, 'a' || 'b'"

same "set-returning functions in the target list, split with no planner state" \
     "SELECT a, generate_series(1, a) FROM t0 WHERE a < 3 ORDER BY 1, 2"

# WHERE false becomes a Result with a one-time filter and no child.  PostgreSQL
# 19's Result says what it stands in for, and EXPLAIN asserts that a Result it
# takes for a gating one has a child: left at zero, the field made EXPLAIN of
# this query take the backend down.
same "WHERE false, as a Result with no child" \
     "SELECT count(*) FROM t0 WHERE false"

has "and EXPLAIN shows it" \
    "EXPLAIN (COSTS OFF) SELECT count(*) FROM t0 WHERE false" "One-Time Filter: false"

# --- what PostgreSQL 18 put in the query ---------------------------------------

# A grouped expression is a Var of an RTE_GROUP entry now, which ORCA's
# translator has never heard of; the port folds it back, as the planner does.
same "GROUP BY an expression" \
     "SELECT a % 3, count(*) FROM t0 GROUP BY a % 3 ORDER BY 1"

same "GROUP BY an output column, and HAVING on a grouped one" \
     "SELECT a FROM t0 GROUP BY a HAVING a > 997 ORDER BY a"

q "CREATE TABLE t0_gen (a int, b int GENERATED ALWAYS AS (a * 2) VIRTUAL);
   INSERT INTO t0_gen (a) VALUES (1), (2);" > /dev/null

declined "a virtual generated column, which the planner expands and ORCA would read as NULL" \
         "SELECT * FROM t0_gen ORDER BY a" "virtual generated columns"

# --- a filter on a Result -----------------------------------------------------
#
# PostgreSQL 19's Result evaluates no qual, and Cloudberry's does: ORCA's
# translator puts every filter it cannot push into a node on a Result above
# it, and on PostgreSQL 19 such a filter was printed by EXPLAIN and never
# applied.  HAVING returned groups it should not have.

same "HAVING, which ORCA filters on a Result" \
     "SELECT a % 3 AS m, count(*) FROM t0 GROUP BY 1 HAVING count(*) > 300 ORDER BY 1"

has "and the filter goes on the aggregate, where the planner puts a HAVING" \
    "EXPLAIN (COSTS OFF) SELECT b, count(*) FROM t0 GROUP BY b HAVING count(*) > 100" \
    "Filter: (count(*) > 100)"

same "a filter over a limit, which no node below can take" \
     "SELECT * FROM (SELECT a, b FROM t0 ORDER BY a LIMIT 20) s WHERE s.b = 'v3' ORDER BY a"

has "and goes under a Subquery Scan, PostgreSQL's node for filtering a plan's rows" \
    "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT a, b FROM t0 ORDER BY a LIMIT 20) s WHERE s.b = 'v3'" \
    "Subquery Scan"

same "ALL, which ORCA turns into a count under a filter" \
     "SELECT a FROM t0 WHERE a > ALL (SELECT a FROM t0 WHERE a < 998) ORDER BY a"

# --- subqueries as SubPlans ---------------------------------------------------
#
# ORCA turns most subqueries into joins, which are T1's; this setting makes it
# keep them as SubPlans, which T0 can plan.

same "EXISTS as a SubPlan" \
     "SELECT a FROM t0 WHERE a < 5 AND EXISTS (SELECT 1 FROM t0 t2 WHERE t2.a = t0.a + 1) ORDER BY a" \
     "SET gp.optimizer_enforce_subplans = on"

# PostgreSQL 19 has no SubLinkType for NOT EXISTS; the translator builds it as
# NOT over an EXISTS SubPlan, which is what the parser writes.
same "NOT EXISTS, which PostgreSQL 19 has no sublink type for" \
     "SELECT a FROM t0 WHERE a < 10 AND NOT EXISTS (SELECT 1 FROM t0 t2 WHERE t2.a = t0.a AND t2.a % 2 = 0) ORDER BY a" \
     "SET gp.optimizer_enforce_subplans = on"

same "IN as a SubPlan" \
     "SELECT a FROM t0 WHERE a IN (SELECT a * 2 FROM t0 WHERE a < 3) ORDER BY a" \
     "SET gp.optimizer_enforce_subplans = on"

same "NOT IN as a SubPlan, NULLs included" \
     "SELECT a FROM t0 WHERE a < 10 AND a NOT IN (SELECT a * 2 FROM t0 WHERE a < 4) ORDER BY a" \
     "SET gp.optimizer_enforce_subplans = on"

same "a correlated scalar subquery" \
     "SELECT a, (SELECT b FROM t0 t2 WHERE t2.a = t0.a + 1) FROM t0 WHERE a < 4 ORDER BY a" \
     "SET gp.optimizer_enforce_subplans = on"

# --- aggregation in stages -----------------------------------------------------

# A partial and a final Agg, with the transition state serialised between
# them for avg(numeric), whose state is internal.
same "two-stage aggregation" \
     "SELECT b, avg(c), count(*), sum(a) FROM t0 GROUP BY b ORDER BY b" \
     "SET gp.optimizer_force_multistage_agg = on"

# PostgreSQL 19 runs an Agg in one split mode; Cloudberry's executor finishes
# each Aggref by its own, and ORCA mixes them in one node here.
declined "an aggregate that mixes stages in one node" \
         "SELECT avg(c), stddev(a), count(DISTINCT b) FROM t0" \
         "mixes aggregation stages" \
         "SET gp.optimizer_force_multistage_agg = on"

# ORCA's core rewrites percentile_cont into Cloudberry's gp_percentile_cont,
# by OID (naucrates/dxl/gpdb_types.h), and PostgreSQL 19 has no such function.
declined "percentile_cont, which ORCA rewrites to a Cloudberry function" \
         "SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY c) FROM t0" \
         "0.9189"

# --- how the plan is used -------------------------------------------------------

same "a prepared statement, custom plans and then the generic one" \
     "EXECUTE p0(10); EXECUTE p0(20); EXECUTE p0(30); EXECUTE p0(40); EXECUTE p0(50); EXECUTE p0(60); EXECUTE p0(70)" \
     "PREPARE p0(int) AS SELECT count(*) FROM t0 WHERE a < \$1"

# An aggregate cannot run backwards, so a scroll cursor over one needs a
# Material on top; standard_planner adds it, and so does the port.
has "a scroll cursor over a plan that cannot run backwards gets a Material" \
    "EXPLAIN (COSTS OFF) DECLARE c0 SCROLL CURSOR FOR SELECT count(*) FROM t0;" \
    "Materialize"

is "and reads backwards" \
   "BEGIN; DECLARE c0 SCROLL CURSOR FOR SELECT count(*) FROM t0;
    FETCH ALL FROM c0; FETCH BACKWARD 1 FROM c0; COMMIT;" "1001
1001"

is "CREATE TABLE AS plans its SELECT, and the table gets the SELECT's types" \
   "CREATE TABLE t0_ctas AS SELECT a, c::numeric(10,2) AS c2 FROM t0 WHERE a < 5;
    SELECT string_agg(attname || ':' || format_type(atttypid, atttypmod), ',' ORDER BY attnum)
      FROM pg_attribute WHERE attrelid = 't0_ctas'::regclass AND attnum > 0;" \
   "a:integer,c2:numeric(10,2)"

# PL/pgSQL takes a fast path for an expression like r := x + 1, which it can
# only take when the plan is a lone Result.
is "PL/pgSQL's simple expressions still take their fast path" \
   "CREATE FUNCTION t0_plus(int) RETURNS int LANGUAGE plpgsql
      AS \$\$ DECLARE r int; BEGIN r := \$1 + 1; RETURN r; END \$\$;
    SELECT t0_plus(41);" "42"

# --- what the plan must still check ----------------------------------------------
#
# ORCA's translator builds a permission entry for each table it scans and
# nothing else, so the port adds the query's own: a view's, and one for any
# table ORCA plans no scan of.  PostgreSQL checks all of them.

q "CREATE TABLE t0_secret (x int); INSERT INTO t0_secret VALUES (42);
   CREATE VIEW t0_view AS SELECT x FROM t0_secret;
   CREATE ROLE t0_reader; GRANT SELECT ON t0, t0_view TO t0_reader;" > /dev/null

is "a view its owner may read is read through" \
   "SET ROLE t0_reader; SELECT * FROM t0_view;" "42"

refused "a table behind WHERE false is still checked" \
        "SET ROLE t0_reader; SELECT count(*) FROM t0_secret WHERE false;" \
        "permission denied for table t0_secret"

q "REVOKE SELECT ON t0_view FROM t0_reader;" > /dev/null
refused "and a view's own privilege is checked" \
        "SET ROLE t0_reader; SELECT * FROM t0_view;" \
        "permission denied for view t0_view"

q "CREATE TABLE t0_rls (owner text, val int);
   INSERT INTO t0_rls VALUES ('t0_reader', 1), ('somebody', 2);
   ALTER TABLE t0_rls ENABLE ROW LEVEL SECURITY;
   CREATE POLICY t0_own ON t0_rls USING (owner = current_user);
   GRANT SELECT ON t0_rls TO t0_reader;" > /dev/null

same "row security applies to ORCA's scan" \
     "SELECT val FROM t0_rls ORDER BY val" "SET ROLE t0_reader"

# --- errors ---------------------------------------------------------------------

# Raised by the constant folding done before ORCA is asked, which is where
# most errors a query can raise at plan time are.
refused "a PostgreSQL error raised before ORCA is asked is the statement's error" \
        "SELECT a FROM t0 WHERE a = 1 / 0;" "division by zero"

# And one raised inside ORCA's task.  The translator checks the query's
# permissions as it translates it (TranslateSelectQueryToDXL), through the
# wrapper layer, so a refusal there is a PostgreSQL error that becomes an ORCA
# exception, unwinds ORCA, and is re-thrown as itself by the C caller -- not
# reported as a failed plan, and not fallen back from.
refused "a PostgreSQL error raised inside ORCA comes back as itself" \
        "SET ROLE t0_reader; SELECT count(*) FROM t0_secret;" \
        "permission denied for table t0_secret"

same "and ORCA plans the next statement in that backend" \
     "SELECT count(*) FROM t0 WHERE a < 50" \
     "SET ROLE t0_reader; DO \$\$ BEGIN PERFORM count(*) FROM t0_secret; EXCEPTION WHEN insufficient_privilege THEN NULL; END \$\$"

# --- what ORCA declines, and says so ---------------------------------------------

has "the trace is Cloudberry's, word for word" \
    "SET gp.optimizer_trace_fallback = on; SELECT a FROM t0 WHERE a = 1 FOR UPDATE;" \
    "GPORCA failed to produce a plan, falling back to Postgres-based planner"

# ORCA ignores row marks, because Cloudberry locks the whole table for them;
# PostgreSQL locks the rows, in a node the plan must have.
declined "FOR UPDATE, which would lock nothing" \
         "SELECT a FROM t0 WHERE a = 1 FOR UPDATE" "FOR UPDATE and FOR SHARE"

# On one node every relation is coordinator-only, and Cloudberry's refusal of
# a coordinator-only table is kept for what it was for, the catalogs.
declined "the system catalogs, as in Cloudberry" \
         "SELECT count(*) FROM pg_class WHERE relname = 't0'" \
         "Queries on master-only tables"

# A data-modifying statement in WITH runs whether or not anything reads it.
# ORCA's CTE producer is a SELECT, and until this was refused ORCA read the
# rows such a statement would change and changed none of them.
declined "a data-modifying statement in WITH" \
         "WITH d AS (DELETE FROM t0_mcte WHERE a <= 5) SELECT 1" \
         "a data-modifying statement in WITH" \
         "CREATE TEMP TABLE t0_mcte AS SELECT generate_series(1, 10) a"

got=$("$PSQL" -X -q -t -A -d postgres \
	-c "CREATE TEMP TABLE t0_mcte AS SELECT generate_series(1, 10) a" \
	-c "WITH d AS (DELETE FROM t0_mcte WHERE a <= 5) SELECT 1" \
	-c "WITH i AS (INSERT INTO t0_mcte VALUES (100)) SELECT 2" \
	-c "SELECT count(*), max(a) FROM t0_mcte" 2>&1)
[ "$got" = "$(printf '1\n2\n6|100')" ] \
	&& ok "and the DELETE and the INSERT in WITH both ran" \
	|| notok "and the DELETE and the INSERT in WITH both ran" "got [$got]"

# The same kind of loss, found by T2: ORCA prunes a computed column nothing
# reads, volatile or not, where the planner keeps a volatile one -- so a
# sequence a subquery's unread column advances was never advanced.
declined "a volatile function in a column nothing reads" \
         "SELECT count(*) FROM (SELECT nextval('t0_seq') n, a FROM t0) x" \
         "a volatile function in a column nothing reads" \
         "CREATE TEMP SEQUENCE t0_seq"

is "and the planner that plans it advances the sequence for every row" \
   "CREATE TEMP SEQUENCE t0_seq; SELECT count(*) FROM (SELECT nextval('t0_seq') n, a FROM t0) x;
    SELECT last_value FROM t0_seq" "$(printf '1001\n1001')"

same "a volatile column the query reads is ORCA's to plan" \
     "SELECT count(*), count(DISTINCT n) FROM (SELECT nextval('t0_seq') n, a FROM t0) x" \
     "CREATE TEMP SEQUENCE t0_seq"

# --- ORCA's memory ----------------------------------------------------------------

# gp.optimizer_use_gpdb_allocators is on, as in Cloudberry, and read now: ORCA
# allocates through PostgreSQL memory contexts, which a leak would show in.
is "ORCA allocates through PostgreSQL memory contexts" \
   "SELECT count(*) > 0 FROM pg_backend_memory_contexts WHERE name = 'GPORCA memory pool';" "t"

# Measured by the planner: a query ORCA plans adds what it looks up to ORCA's
# metadata cache, which is ORCA's to keep, and since T2 ORCA plans the
# measurement itself -- pg_backend_memory_contexts is a function in FROM.  A
# leak is growth while nothing new is asked.
is "and a thousand plans later it holds no more than it did" \
   "DO \$\$ DECLARE n bigint; BEGIN
      FOR i IN 1..1000 LOOP EXECUTE format('SELECT count(*) FROM t0 WHERE a < %s GROUP BY b LIMIT 1', i) INTO n; END LOOP; END \$\$;
    SET gp.optimizer = off;
    CREATE TEMP TABLE t0_mem AS SELECT sum(total_bytes) AS b FROM pg_backend_memory_contexts WHERE name LIKE 'GPORCA%';
    RESET gp.optimizer;
    DO \$\$ DECLARE n bigint; BEGIN
      FOR i IN 1..1000 LOOP EXECUTE format('SELECT count(*) FROM t0 WHERE a < %s GROUP BY b LIMIT 1', i) INTO n; END LOOP; END \$\$;
    SET gp.optimizer = off;
    SELECT sum(total_bytes) <= (SELECT b FROM t0_mem) FROM pg_backend_memory_contexts WHERE name LIKE 'GPORCA%';" "t"

echo
echo "22. joins, index and bitmap scans, Append, windows and CTEs, planned by ORCA"

# T1 of the translator.  Most checks are "shape": ORCA planned it, the plan
# has the node the check is about, and the answer is the planner's.  The
# middle clause is the one "same" does not have, and what it guards against
# is a check that passes through a plan that went around the node it names.

# shape <name> <node> <query> [setup]
shape() {
	local setup="${4:-SELECT}" orca pg plan
	plan=$(q2 "$setup" "EXPLAIN (COSTS OFF, VERBOSE) $3")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tail -3 | tr '\n' '|')"; return ;;
	esac
	case "$plan" in
		*"$2"*) ;;
		*) notok "$1" "no [$2] in the plan: $(printf '%s' "$plan" | tr '\n' '|')"; return ;;
	esac
	orca=$(q2 "$setup" "$3")
	pg=$(q2 "$setup; SET gp.optimizer = off" "$3")
	[ "$orca" = "$pg" ] && ok "$1" || notok "$1" "orca [$orca], planner [$pg]"
}

q "CREATE TABLE t1a (i int, j int, t text);
   CREATE TABLE t1b (i int, k int, u text);
   INSERT INTO t1a SELECT g, g % 10, 'a' || (g % 7) FROM generate_series(1, 2000) g;
   INSERT INTO t1a VALUES (NULL, NULL, NULL), (NULL, 3, 'x');
   INSERT INTO t1b SELECT g * 2, g % 5, 'b' || (g % 3) FROM generate_series(1, 1000) g;
   INSERT INTO t1b VALUES (NULL, NULL, NULL), (4, NULL, 'y');
   CREATE INDEX t1a_i ON t1a (i);
   CREATE INDEX t1a_j_i ON t1a (j, i);
   CREATE INDEX t1b_i ON t1b (i);" > /dev/null
# VACUUM, for the visibility map an index-only scan is costed by; one
# statement each, since a -c string runs in one transaction and VACUUM will
# not run in one.
q "VACUUM ANALYZE t1a;" > /dev/null
q "VACUUM ANALYZE t1b;" > /dev/null

# --- joins ------------------------------------------------------------------

shape "a hash join" "Hash Join" \
      "SELECT t1a.i, t1b.k FROM t1a JOIN t1b ON t1a.i = t1b.i WHERE t1a.j < 3 ORDER BY 1, 2"

shape "a left join, with its NULLs" "Left Join" \
      "SELECT t1a.i, t1b.k FROM t1a LEFT JOIN t1b ON t1a.i = t1b.i WHERE t1a.i < 20 OR t1a.i IS NULL ORDER BY 1, 2"

same "a right join" \
     "SELECT t1a.i, t1b.k FROM t1a RIGHT JOIN t1b ON t1a.i = t1b.i ORDER BY 2, 1 LIMIT 20"

shape "a full join" "Full Join" \
      "SELECT t1a.i, t1b.i FROM t1a FULL JOIN t1b ON t1a.i = t1b.i WHERE coalesce(t1a.i, t1b.i) < 20 OR t1a.i IS NULL OR t1b.i IS NULL ORDER BY 1, 2"

shape "EXISTS, as a semi-join" "Semi Join" \
      "SELECT count(*) FROM t1a WHERE EXISTS (SELECT 1 FROM t1b WHERE t1b.i = t1a.i)"

shape "NOT EXISTS, as an anti-join" "Anti Join" \
      "SELECT count(*) FROM t1a WHERE NOT EXISTS (SELECT 1 FROM t1b WHERE t1b.i = t1a.i)"

same "IN" \
     "SELECT count(*) FROM t1a WHERE t1a.i IN (SELECT i FROM t1b)"

shape "a nested loop, on an inequality" "Nested Loop" \
      "SELECT count(*) FROM t1a JOIN t1b ON t1a.i < t1b.i WHERE t1a.i < 10 AND t1b.i < 30"

# ORCA merge-joins only a full join -- CXformImplementFullOuterMergeJoin is its
# one merge join -- so that is the one shape with no hash join to fall to.
shape "a merge join" "Merge Full Join" \
      "SELECT t1a.i, t1b.i FROM t1a FULL JOIN t1b ON t1a.i = t1b.i WHERE coalesce(t1a.i, t1b.i) < 20 OR t1a.i IS NULL OR t1b.i IS NULL ORDER BY 1, 2" \
      "SET gp.optimizer_enable_hashjoin = off"

shape "an index nested loop, its parameter set for each outer row" "Index Cond: (t1b.i = t1a.i)" \
      "SELECT t1a.i, t1b.k FROM t1a JOIN t1b ON t1a.i = t1b.i WHERE t1a.j = 1 ORDER BY 1" \
      "SET gp.optimizer_enable_hashjoin = off; SET gp.optimizer_enable_mergejoin = off"

same "three tables" \
     "SELECT count(*) FROM t1a JOIN t1b ON t1a.i = t1b.i JOIN t1a a2 ON a2.j = t1b.k WHERE t1a.j < 2"

# PostgreSQL 19 has no anti-join that answers NOT IN's NULLs, and ORCA's
# other plan for it -- an apply kept as a SubPlan, when the two transforms
# that make the join are turned off -- was measured at T1 at 6.8 seconds
# against the planner's 6 milliseconds.  So it is declined, and counted.
declined "NOT IN, which ORCA makes an anti-join PostgreSQL 19 cannot run" \
         "SELECT count(*) FROM t1a WHERE t1a.j NOT IN (SELECT k FROM t1b)" \
         "NOT IN as an anti-join"

# --- IS NOT DISTINCT FROM ------------------------------------------------------
#
# PostgreSQL 19's hash join never lets a NULL key meet another, and has no
# field of Cloudberry's to check NULLs apart; the port hashes each side with its
# own hash function and a NULL as 0, and leaves the condition to tell a NULL
# from a 0.  INTERSECT and EXCEPT are hash joins on it, over every column.

shape "INTERSECT, whose NULLs meet in a hash join" "IS DISTINCT FROM" \
      "SELECT j FROM t1a INTERSECT SELECT k FROM t1b ORDER BY 1"

same "EXCEPT, NULLs included" \
     "SELECT j FROM t1a EXCEPT SELECT k FROM t1b ORDER BY 1"

same "IS NOT DISTINCT FROM between two integer types" \
     "SELECT count(*) FROM t1a JOIN t1b ON t1a.j IS NOT DISTINCT FROM t1b.k::int8"

same "and over text" \
     "SELECT count(*) FROM t1a JOIN t1b ON t1a.t IS NOT DISTINCT FROM t1b.u"

# A value of the key's own type in a NULL's place would be simpler, and would
# not exist for every type: an empty varlena is not a numeric.
same "and over numeric, which has no value to stand in for a NULL" \
     "SELECT count(*) FROM t1a JOIN t1b ON (t1a.j::numeric) IS NOT DISTINCT FROM (t1b.k::numeric)"

same "a 0 still told apart from a NULL" \
     "SELECT count(*) FROM (VALUES (0), (NULL), (1)) x(v) JOIN (VALUES (NULL::int), (0)) y(w) ON x.v IS NOT DISTINCT FROM y.w"

# --- Append -------------------------------------------------------------------

shape "UNION ALL, as an Append" "Append" \
      "SELECT i FROM t1a WHERE i < 5 UNION ALL SELECT i FROM t1b WHERE i < 10 ORDER BY 1"

same "UNION" \
     "SELECT j FROM t1a UNION SELECT k FROM t1b ORDER BY 1"

# --- index scans ------------------------------------------------------------------

shape "an index scan" "Index Scan" \
      "SELECT * FROM t1a WHERE i = 17"

shape "an index-only scan" "Index Only Scan" \
      "SELECT j, i FROM t1a WHERE j = 3 AND i < 50 ORDER BY 1, 2" \
      "SET gp.optimizer_enable_indexscan = off"

shape "a bitmap scan, two ranges ORed" "BitmapOr" \
      "SELECT count(*) FROM t1a WHERE i < 30 OR i > 1990"

same "a prepared index scan, custom plans and then the generic one" \
     "EXECUTE p1(10); EXECUTE p1(20); EXECUTE p1(30); EXECUTE p1(40); EXECUTE p1(50); EXECUTE p1(60); EXECUTE p1(70)" \
     "PREPARE p1(int) AS SELECT j FROM t1a WHERE i = \$1"

# A lossy index says a match may not be one, and PostgreSQL 15's
# IndexOnlyScan.recheckqual is what checks it against the index tuple.  GiST's
# point <@ polygon is lossy: the index answers for the bounding box.
q "CREATE TABLE t1p (p point);
   INSERT INTO t1p SELECT point(g % 10, g / 10) FROM generate_series(0, 99) g;
   CREATE INDEX t1p_p ON t1p USING gist (p);" > /dev/null
q "VACUUM ANALYZE t1p;" > /dev/null

# The triangle holds 55 of the grid's points and its bounding box all 100, so
# an index-only scan that skipped the recheck would answer 100.
shape "a lossy GiST index-only scan, whose matches are rechecked" "Index Only Scan" \
      "SELECT p[0], p[1] FROM t1p WHERE p <@ polygon '((0,0),(9,0),(0,9))' ORDER BY 1, 2"

# The planner skips an index that this transaction's snapshots may not read
# through yet, and marks its plan transient; ORCA's metadata admits it, so the
# port refuses a plan that uses one.  Such an index is built over a HOT chain
# that a snapshot may still need the old end of -- the simplest being a chain
# the building transaction itself broke -- and the fixture is checked before
# the test relies on it.  fillfactor leaves the page room for the HOT update.
q "CREATE TABLE t1_hot (a int, b int) WITH (fillfactor = 50);
   INSERT INTO t1_hot SELECT g, g FROM generate_series(1, 5000) g;
   ANALYZE t1_hot;" > /dev/null

is "an index built over a HOT chain its own transaction broke is marked so" \
   "BEGIN; UPDATE t1_hot SET a = a + 100000 WHERE b = 5;
    CREATE INDEX t1_hot_a ON t1_hot (a);
    SELECT indcheckxmin FROM pg_index WHERE indexrelid = 't1_hot_a'::regclass;
    ROLLBACK;" "t"

got=$(q "SET gp.optimizer_trace_fallback = on;
         BEGIN; UPDATE t1_hot SET a = a + 100000 WHERE b = 5;
         CREATE INDEX t1_hot_a ON t1_hot (a);
         SELECT b FROM t1_hot WHERE a = 100005; ROLLBACK;")
case "$got" in
	*"an index newer than this transaction's snapshots"*5*) ok "and a plan through it, in that transaction, goes to the planner" ;;
	*) notok "and a plan through it, in that transaction, goes to the planner" "$got" ;;
esac

q "BEGIN; UPDATE t1_hot SET a = a + 100000 WHERE b = 5;
   CREATE INDEX t1_hot_a ON t1_hot (a); COMMIT;" > /dev/null
shape "and once it is old enough, ORCA uses it" "Index" \
      "SELECT b FROM t1_hot WHERE a = 100005"

# ORCA plans no index or bitmap scan of a relation with security quals -- its
# transforms decline such a Get -- so the table scan, which applies them, is
# the only scan that meets them.
q "CREATE TABLE t1_rls (owner text, val int);
   INSERT INTO t1_rls SELECT CASE WHEN g % 2 = 0 THEN 't1_reader' ELSE 'other' END, g
     FROM generate_series(1, 5000) g;
   CREATE INDEX t1_rls_val ON t1_rls (val);
   ALTER TABLE t1_rls ENABLE ROW LEVEL SECURITY;
   CREATE POLICY t1_own ON t1_rls USING (owner = current_user);
   CREATE ROLE t1_reader; GRANT SELECT ON t1_rls TO t1_reader;
   ANALYZE t1_rls;" > /dev/null

same "row security applies where an index would have been used" \
     "SELECT val FROM t1_rls WHERE val BETWEEN 10 AND 20 ORDER BY val" "SET ROLE t1_reader"

# --- window functions --------------------------------------------------------------

shape "a window function" "WindowAgg" \
      "SELECT i, j, rank() OVER (PARTITION BY j ORDER BY i) FROM t1a WHERE i < 30 ORDER BY 1"

same "a ROWS frame" \
     "SELECT i, sum(i) OVER (ORDER BY i ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) FROM t1a WHERE i < 20 ORDER BY 1"

same "a RANGE frame with offsets" \
     "SELECT i, sum(i) OVER (ORDER BY i RANGE BETWEEN 3 PRECEDING AND 1 FOLLOWING) FROM t1a WHERE i < 15 ORDER BY 1"

same "OVER ()" \
     "SELECT i, count(*) OVER () FROM t1a WHERE i < 5 ORDER BY 1"

# PostgreSQL 18 made EXPLAIN print every window's definition under its name.
has "EXPLAIN names each window, as it now always does" \
    "EXPLAIN (COSTS OFF, VERBOSE) SELECT i, rank() OVER (ORDER BY i) FROM t1a WHERE i < 3" \
    "Window: w1 AS"

# ORCA can put a filter on any window node; PostgreSQL 19 asserts that only
# the top one has a qual, which is about run conditions, and ORCA makes none.
same "a filter over a window function" \
     "SELECT * FROM (SELECT i, rank() OVER (ORDER BY i) r FROM t1a WHERE i < 50) s WHERE r < 4 ORDER BY 1"

# transformGroupedWindows(), before ORCA: the grouping goes into a subquery.
shape "window functions over a GROUP BY, split in two before ORCA sees them" "WindowAgg" \
      "SELECT j, count(*), rank() OVER (ORDER BY count(*) DESC, j) FROM t1a GROUP BY j ORDER BY 1"

same "and a window over an aggregate, with a HAVING" \
     "SELECT j, sum(count(*)) OVER (ORDER BY j) FROM t1a GROUP BY j HAVING count(*) > 100 ORDER BY 1"

# PostgreSQL 19 added IGNORE NULLS, which ORCA's window reference has no
# field for: planned, it would come back as RESPECT NULLS.
declined "IGNORE NULLS, which ORCA's window reference cannot carry" \
         "SELECT i, lag(j) IGNORE NULLS OVER (ORDER BY i) FROM t1a WHERE i < 10 ORDER BY 1" \
         "window function with IGNORE NULLS"

# --- CTEs ------------------------------------------------------------------------------
#
# A CTE producer is a subplan run by an initplan, and each consumer a CTE Scan
# of it, as the planner makes a CTE; Cloudberry's ShareInputScan and Sequence
# are not PostgreSQL 19's.

shape "a CTE read twice" "CTE Scan" \
      "WITH c AS (SELECT j, count(*) n FROM t1a GROUP BY j) SELECT x.j, y.n FROM c x JOIN c y ON x.j = y.j ORDER BY 1"

same "a CTE whose readers read different columns" \
     "WITH c AS (SELECT i, j, t FROM t1a WHERE i < 100) SELECT x.i, y.t FROM c x JOIN c y ON x.i = y.j ORDER BY 1, 2"

# A CTE reader in DXL has no filter of its own, so ORCA puts one on a Result
# above it, which the port moves into the CTE Scan -- a scan tests a qual on
# the rows it is about to return -- rather than under a Subquery Scan.
got=$(q "EXPLAIN (COSTS OFF) WITH c AS (SELECT i, j, t FROM t1a WHERE i < 100)
         SELECT x.i, y.t FROM c x JOIN c y ON x.i = y.j")
case "$got" in
	*"Subquery Scan"*) notok "and a filter on a reader goes into its CTE Scan" "$got" ;;
	*"CTE Scan"*"Filter:"*) ok "and a filter on a reader goes into its CTE Scan" ;;
	*) notok "and a filter on a reader goes into its CTE Scan" "$got" ;;
esac

same "a CTE that reads a CTE" \
     "WITH c1 AS (SELECT j FROM t1a WHERE i < 50), c2 AS (SELECT j, count(*) n FROM c1 GROUP BY j)
      SELECT * FROM c2 x JOIN c2 y USING (j) JOIN c1 z USING (j) ORDER BY 1"

same "a CTE inside a subquery" \
     "SELECT count(*) FROM (WITH c AS (SELECT k FROM t1b WHERE k > 2) SELECT * FROM c c1 JOIN c c2 USING (k)) s"

shape "several DISTINCT aggregates, which ORCA answers with CTEs of its own" "CTE Scan" \
      "SELECT count(DISTINCT j), count(DISTINCT t), count(*) FROM t1a" \
      "SET gp.optimizer_enable_multiple_distinct_aggs = on"

declined "a CTE with an outer reference, which ORCA declines itself" \
         "SELECT i, (WITH c AS (SELECT k FROM t1b WHERE t1b.i = t1a.i) SELECT count(*) FROM c c1, c c2) FROM t1a WHERE i < 8 ORDER BY 1" \
         "CTE with outer references" \
         "SET gp.optimizer_enforce_subplans = on"

echo
echo "23. INSERT, UPDATE and DELETE, assertions, functions in FROM and foreign tables, planned by ORCA"

# T2 of the translator.  A statement that changes rows is checked by what it
# leaves: "dml" runs it under ORCA and under the planner, each in a session
# of its own from the same setup -- temporary tables, so the two do not see
# each other -- and compares what a query afterwards reads, after checking
# that ORCA planned it and that its plan has the node the check is about.

# dml <name> <node> <statement> <check> <setup>
dml() {
	local plan orca pg
	plan=$(q2 "$5" "EXPLAIN (COSTS OFF, VERBOSE) $3")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tail -3 | tr '\n' '|')"; return ;;
	esac
	case "$plan" in
		*"$2"*) ;;
		*) notok "$1" "no [$2] in the plan: $(printf '%s' "$plan" | tr '\n' '|')"; return ;;
	esac
	orca=$("$PSQL" -X -q -t -A -d postgres -c "$5" -c "$3" -c "$4" 2>&1)
	pg=$("$PSQL" -X -q -t -A -d postgres -c "$5; SET gp.optimizer = off" -c "$3" -c "$4" 2>&1)
	[ "$orca" = "$pg" ] && ok "$1" || notok "$1" "orca [$orca], planner [$pg]"
}

# raised <name> <statements> <error>: ORCA planned them, and running them
# raised that error.
raised() {
	local got; got=$(q "SET gp.optimizer_trace_fallback = on; $2")
	case "$got" in
		*"GPORCA failed"*) notok "$1" "not planned by ORCA: $got" ;;
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected an error containing [$3], got [$got]" ;;
	esac
}

T2D="CREATE TEMP TABLE t2d AS SELECT g AS a, g % 10 AS b, 'x' || g AS c FROM generate_series(1, 100) g"

# --- rows written -----------------------------------------------------------

dml "an INSERT of values" "Insert on" \
    "INSERT INTO t2d VALUES (1000, 1, 'y'), (1001, NULL, NULL)" \
    "SELECT count(*), sum(a), count(b), count(c) FROM t2d" "$T2D"

dml "an INSERT of a query's rows, from its own target" "Insert on" \
    "INSERT INTO t2d SELECT a + 1000, b, c FROM t2d WHERE b = 3" \
    "SELECT count(*), sum(a) FROM t2d" "$T2D"

dml "an UPDATE, in place" "Update on" \
    "UPDATE t2d SET b = b + 100, c = upper(c) WHERE a <= 10" \
    "SELECT count(*), sum(b), max(c) FROM t2d WHERE b >= 100" "$T2D"

dml "a DELETE" "Delete on" \
    "DELETE FROM t2d WHERE a > 90 OR b = 0" \
    "SELECT count(*), sum(a) FROM t2d" "$T2D"

dml "an UPDATE through an index" "Update on" \
    "UPDATE t2d SET c = 'hit' WHERE a = 5000" \
    "SELECT a, c FROM t2d WHERE c = 'hit'" \
    "CREATE TEMP TABLE t2d AS SELECT g AS a, g % 10 AS b, 'x' || g AS c FROM generate_series(1, 20000) g;
     CREATE INDEX ON t2d (a); ANALYZE t2d"

# The columns an UPDATE sets, and no others, as the planner's plan names
# them: ModifyTable takes the rest from the old row, and a reader of the plan
# takes updateColnos as what the statement changes -- gp_sql's guard on a
# directory table refused an UPDATE of its tag when every column was named.
has "an UPDATE passes the columns it sets and the row's ctid, as the planner's does" \
    "$T2D; EXPLAIN (COSTS OFF, VERBOSE) UPDATE t2d SET b = b + 1 WHERE a = 1" \
    "Output: (t2d.b + 1), t2d.ctid"

# PostgreSQL 19 has no gp_segment_id, and the port gives ORCA tableoid in its
# place; ModifyTable finds the row by ctid alone, and the stand-in goes no
# further than ORCA.
got=$(q2 "$T2D" "EXPLAIN (COSTS OFF, VERBOSE) UPDATE t2d SET b = 0 WHERE a = 1")
case "$got" in
	*tableoid*|*gp_segment_id*) notok "the row is found by ctid, with nothing in gp_segment_id's place" "$got" ;;
	*"t2d.ctid"*) ok "the row is found by ctid, with nothing in gp_segment_id's place" ;;
	*) notok "the row is found by ctid, with nothing in gp_segment_id's place" "$got" ;;
esac

dml "a dropped column, which the executor wants a NULL for" "Insert on" \
    "INSERT INTO t2x VALUES (3, 30); UPDATE t2x SET b = b + 1 WHERE a > 1" \
    "SELECT * FROM t2x ORDER BY a" \
    "CREATE TEMP TABLE t2x (a int, gone text, b int); INSERT INTO t2x VALUES (1, 'g', 10), (2, 'g', 20);
     ALTER TABLE t2x DROP COLUMN gone"

# The executor computes a stored generated column, and insists on a NULL
# constant for it in an INSERT's plan; an UPDATE recomputes it from the
# columns the statement set, which ORCA's permission entry did not name.
dml "a stored generated column, computed on INSERT and on UPDATE" "Update on" \
    "UPDATE t2g SET a = a + 10 WHERE a = 1" \
    "INSERT INTO t2g (a) VALUES (3); SELECT * FROM t2g ORDER BY a" \
    "CREATE TEMP TABLE t2g (a int, g int GENERATED ALWAYS AS (a * 2) STORED); INSERT INTO t2g (a) VALUES (1), (2)"

# --- constraints, which ModifyTable checks, not an Assert --------------------
#
# ORCA puts an Assert for a table's NOT NULL and CHECK constraints under the
# DML node.  ModifyTable checks both itself, after the BEFORE triggers that
# may change the row, so the Assert is left out.

T2N="CREATE TEMP TABLE t2n (a int PRIMARY KEY, b int NOT NULL CHECK (b > 0))"

got=$(q2 "$T2N" "EXPLAIN (COSTS OFF) INSERT INTO t2n VALUES (1, 1)")
case "$got" in
	*Assert*) notok "no Assert for the constraints ModifyTable checks" "$got" ;;
	*"Optimizer: GPORCA"*) ok "no Assert for the constraints ModifyTable checks" ;;
	*) notok "no Assert for the constraints ModifyTable checks" "$got" ;;
esac

raised "a NOT NULL violation, in PostgreSQL's words" \
       "$T2N; INSERT INTO t2n VALUES (2, NULL)" \
       "null value in column \"b\" of relation \"t2n\" violates not-null constraint"

raised "a CHECK violation, likewise" \
       "$T2N; INSERT INTO t2n VALUES (1, 1); UPDATE t2n SET b = 0" \
       "new row for relation \"t2n\" violates check constraint \"t2n_b_check\""

dml "a BEFORE trigger mends a row before the constraint sees it" "Insert on" \
    "INSERT INTO t2n VALUES (4, NULL)" \
    "SELECT * FROM t2n" \
    "$T2N; CREATE FUNCTION pg_temp.t2_fix() RETURNS trigger LANGUAGE plpgsql AS
       'BEGIN IF NEW.b IS NULL THEN NEW.b := 42; END IF; RETURN NEW; END';
     CREATE TRIGGER t2_fix BEFORE INSERT ON t2n FOR EACH ROW EXECUTE FUNCTION pg_temp.t2_fix()"

# The planner's NULL for a column an INSERT leaves out has been through its
# domain's constraints; a bare NULL of the domain's type would be stored.
raised "an omitted column of a NOT NULL domain is refused, as the planner refuses it" \
       "CREATE DOMAIN t2_nn AS int NOT NULL; CREATE TEMP TABLE t2m (a int, d t2_nn);
        INSERT INTO t2m (a) VALUES (1)" \
       "domain t2_nn does not allow null values"

# --- triggers, rules and the permission entry -----------------------------------

dml "an UPDATE OF trigger fires when its column changes, and only then" "Update on" \
    "UPDATE t2u SET c = 5; UPDATE t2u SET b = 6" \
    "SELECT * FROM t2u_log" \
    "CREATE TEMP TABLE t2u (a int, b int, c int); CREATE TEMP TABLE t2u_log (msg text);
     INSERT INTO t2u VALUES (1, 1, 1);
     CREATE FUNCTION pg_temp.t2_log() RETURNS trigger LANGUAGE plpgsql AS
       'BEGIN INSERT INTO t2u_log VALUES (OLD.b || ''->'' || NEW.b); RETURN NEW; END';
     CREATE TRIGGER t2_log AFTER UPDATE OF b ON t2u FOR EACH ROW EXECUTE FUNCTION pg_temp.t2_log()"

dml "a statement trigger's transition table has the rows" "Insert on" \
    "INSERT INTO t2s SELECT generate_series(1, 7)" \
    "SELECT * FROM t2s_log" \
    "CREATE TEMP TABLE t2s (a int); CREATE TEMP TABLE t2s_log (n bigint);
     CREATE FUNCTION pg_temp.t2_count() RETURNS trigger LANGUAGE plpgsql AS
       'BEGIN INSERT INTO t2s_log SELECT count(*) FROM newtab; RETURN NULL; END';
     CREATE TRIGGER t2_count AFTER INSERT ON t2s REFERENCING NEW TABLE AS newtab
       FOR EACH STATEMENT EXECUTE FUNCTION pg_temp.t2_count()"

dml "a rule's DO ALSO action runs beside the INSERT" "Insert on" \
    "INSERT INTO t2r VALUES (1), (2)" \
    "SELECT * FROM t2r_log ORDER BY a" \
    "CREATE TEMP TABLE t2r (a int); CREATE TEMP TABLE t2r_log (a int);
     CREATE RULE t2r_also AS ON INSERT TO t2r DO ALSO INSERT INTO t2r_log VALUES (NEW.a)"

dml "an INSERT into a partitioned table routes each row" "Insert on" \
    "INSERT INTO t2p SELECT g, 'p' || g FROM generate_series(1, 199) g" \
    "SELECT tableoid::regclass, count(*) FROM t2p GROUP BY 1 ORDER BY 1" \
    "CREATE TEMP TABLE t2p (a int, b text) PARTITION BY RANGE (a);
     CREATE TEMP TABLE t2p1 PARTITION OF t2p FOR VALUES FROM (0) TO (100);
     CREATE TEMP TABLE t2p2 PARTITION OF t2p FOR VALUES FROM (100) TO (200)"

dml "an UPDATE through a view" "Update on" \
    "UPDATE t2v SET b = b * 100; DELETE FROM t2v WHERE a = 10" \
    "SELECT * FROM t2vt ORDER BY a" \
    "CREATE TEMP TABLE t2vt AS SELECT g AS a, g AS b FROM generate_series(1, 10) g;
     CREATE TEMP VIEW t2v AS SELECT a, b FROM t2vt WHERE a > 5"

dml "a prepared UPDATE, past the plan cache's switch to a generic plan" "Update on" \
    "EXECUTE t2up(1, 11); EXECUTE t2up(2, 22); EXECUTE t2up(3, 33);
     EXECUTE t2up(4, 44); EXECUTE t2up(6, 66); EXECUTE t2up(7, 77)" \
    "SELECT * FROM t2d WHERE a <= 7 ORDER BY a" \
    "$T2D; PREPARE t2up(int, int) AS UPDATE t2d SET b = \$2 WHERE a = \$1"

# Row-level security's USING clause is the target's security qual, which
# ORCA's scan applies; its WITH CHECK is a check option, which is refused.
q "DROP TABLE IF EXISTS t2_rls; DROP ROLE IF EXISTS t2_alice;
   CREATE ROLE t2_alice; CREATE TABLE t2_rls (a int, owner text);
   ALTER TABLE t2_rls ENABLE ROW LEVEL SECURITY;
   CREATE POLICY t2_own ON t2_rls USING (owner = current_user);
   GRANT ALL ON t2_rls TO t2_alice;" > /dev/null
T2RLS="TRUNCATE t2_rls; INSERT INTO t2_rls VALUES (1, 't2_alice'), (2, 'bob'), (3, 't2_alice'); SET ROLE t2_alice"
dml "a DELETE under a row-level security policy deletes only the rows it may see" "Delete on" \
    "DELETE FROM t2_rls WHERE a > 0" \
    "RESET ROLE; SELECT * FROM t2_rls ORDER BY a" "$T2RLS"

# --- EvalPlanQual -----------------------------------------------------------------
#
# Under READ COMMITTED, an UPDATE or DELETE that waits for another
# transaction's update of its row re-runs its plan over the new version.
# Every node below ModifyTable depends on its EvalPlanQual parameter, so that
# a second re-check in the same statement starts the plan again.

# epq <name> <statement> <other transaction's update> <check>
epq() {
	local plan orca pg opt res
	plan=$(q2 "SELECT" "EXPLAIN (COSTS OFF) $2")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tail -3 | tr '\n' '|')"; return ;;
	esac
	for opt in on off; do
		q "TRUNCATE t2e; INSERT INTO t2e SELECT g, g FROM generate_series(1, 5) g;" > /dev/null
		( "$PSQL" -X -q -d postgres -c "BEGIN" -c "$3" -c "SELECT pg_sleep(1)" -c "COMMIT" > /dev/null 2>&1 ) &
		# until the other transaction holds the row, as it sleeps
		local tries=0
		until [ "$(q "SELECT count(*) FROM pg_stat_activity WHERE query = 'SELECT pg_sleep(1)' AND state = 'active'")" = 1 ]; do
			tries=$((tries + 1))
			[ "$tries" -gt 400 ] && { notok "$1" "the other transaction never started"; wait; return; }
			sleep 0.05
		done
		"$PSQL" -X -q -d postgres -c "SET gp.optimizer = $opt" -c "$2" > /dev/null 2>&1
		wait
		res=$(q "$4")
		if [ "$opt" = on ]; then orca=$res; else pg=$res; fi
	done
	[ "$orca" = "$pg" ] && ok "$1" || notok "$1" "orca [$orca], planner [$pg]"
}

q "CREATE TABLE t2e (a int, b int); CREATE INDEX ON t2e (a);" > /dev/null

epq "an UPDATE that waited acts on the new version of its row" \
    "UPDATE t2e SET b = b * 10 WHERE a = 1" \
    "UPDATE t2e SET b = b + 100 WHERE a = 1" \
    "SELECT a, b FROM t2e ORDER BY a"

epq "and skips a row that no longer qualifies" \
    "UPDATE t2e SET b = -1 WHERE b = 2" \
    "UPDATE t2e SET b = 100 WHERE a = 2" \
    "SELECT a, b FROM t2e ORDER BY a"

epq "a DELETE that waited deletes the new version" \
    "DELETE FROM t2e WHERE b >= 3" \
    "UPDATE t2e SET b = 50 WHERE a = 3" \
    "SELECT a, b FROM t2e ORDER BY a"

epq "two rows re-checked in one statement" \
    "UPDATE t2e SET b = b * 2 WHERE a <= 3" \
    "UPDATE t2e SET b = b + 1000 WHERE a IN (1, 2)" \
    "SELECT a, b FROM t2e ORDER BY a"

# --- what is refused --------------------------------------------------------------

declined "an UPDATE that reads another relation, which row marks would re-read" \
         "UPDATE t2d SET b = 0 FROM t0 WHERE t2d.a = t0.a" \
         "an UPDATE or DELETE that reads another relation" "$T2D"

declined "a DELETE with a subquery, which ORCA would make a join" \
         "DELETE FROM t2d WHERE a IN (SELECT a FROM t0 WHERE a < 5)" \
         "an UPDATE or DELETE that reads another relation" "$T2D"

declined "an UPDATE of a partitioned table, as in Cloudberry" \
         "UPDATE t2p SET b = 'u' WHERE a = 5" \
         "DML(update) on partitioned tables" \
         "CREATE TEMP TABLE t2p (a int, b text) PARTITION BY RANGE (a);
          CREATE TEMP TABLE t2p1 PARTITION OF t2p FOR VALUES FROM (0) TO (100)"

declined "RETURNING" \
         "INSERT INTO t2d VALUES (1000, 1, 'r') RETURNING a, c" "RETURNING clause" "$T2D"

declined "ON CONFLICT" \
         "INSERT INTO t2d VALUES (1, 1, 'r') ON CONFLICT (a) DO UPDATE SET c = 'conflict'" \
         "ON CONFLICT clause" "$T2D; CREATE UNIQUE INDEX ON t2d (a)"

declined "a view's INSTEAD OF trigger, which needs the whole old row" \
         "INSERT INTO t2vi VALUES (42, 42)" "a view's INSTEAD OF trigger" \
         "CREATE TEMP TABLE t2vt (a int, b int); CREATE TEMP VIEW t2vi AS SELECT * FROM t2vt;
          CREATE FUNCTION pg_temp.t2_vi() RETURNS trigger LANGUAGE plpgsql AS
            'BEGIN INSERT INTO t2vt VALUES (NEW.a, NEW.b); RETURN NEW; END';
          CREATE TRIGGER t2_vi INSTEAD OF INSERT ON t2vi FOR EACH ROW EXECUTE FUNCTION pg_temp.t2_vi()"

declined "an identity column's next value, which nextval() would take a privilege for" \
         "INSERT INTO t2i (v) VALUES ('a')" "an identity column's next value" \
         "CREATE TEMP TABLE t2i (id int GENERATED ALWAYS AS IDENTITY, v text)"

declined "row-level security's WITH CHECK, a check option" \
         "UPDATE t2_rls SET a = a + 10" "View with WITH CHECK OPTION" "$T2RLS"

declined "MERGE" \
         "MERGE INTO t2d USING (SELECT 1 AS a) s ON t2d.a = s.a WHEN MATCHED THEN UPDATE SET c = 'm'" \
         "MERGE command" "$T2D"

# --- Assert -------------------------------------------------------------------------
#
# A scalar subquery ORCA turns into a join is checked for giving at most one
# row by an Assert, Cloudberry's AssertOp, a CustomScan here.  It raises the
# planner's error for the same case.

q "CREATE TABLE t2a (a int, b int); INSERT INTO t2a VALUES (1, 1), (1, 2), (2, 3); ANALYZE t2a;" > /dev/null

shape "a scalar subquery is checked for one row" $'->  Assert\n' \
      "SELECT a, (SELECT b FROM t2a WHERE a = 2) FROM t2a ORDER BY 1"

has "and EXPLAIN shows the test" \
    "EXPLAIN (COSTS OFF) SELECT (SELECT b FROM t2a WHERE a = 2) FROM t2a" "Assert Cond:"

got=$(q "EXPLAIN (COSTS OFF) SELECT (SELECT b FROM t2a WHERE a = 1) FROM t2a")
refused "and more than one row is the planner's error" \
        "SELECT (SELECT b FROM t2a WHERE a = 1) FROM t2a" \
        "more than one row returned by a subquery used as an expression"
case "$got" in
	*"->  Assert"$'\n'*"Optimizer: GPORCA"*) ok "raised by ORCA's plan, not the planner's" ;;
	*) notok "raised by ORCA's plan, not the planner's" "$got" ;;
esac

# --- functions in FROM --------------------------------------------------------------

shape "a function in FROM" "Function Scan" \
      "SELECT g FROM generate_series(1, 5) g WHERE g % 2 = 1"

same "one with a column definition list" \
     "SELECT * FROM json_to_recordset('[{\"a\":1,\"b\":\"x\"},{\"a\":2,\"b\":\"y\"}]') AS x(a int, b text) ORDER BY a"

same "a set-returning SQL function, called rather than inlined" \
     "SELECT a, b FROM pg_temp.t2_rows() ORDER BY a" \
     "CREATE FUNCTION pg_temp.t2_rows() RETURNS TABLE (a int, b text) LANGUAGE sql AS
        'SELECT g, ''r'' || g FROM generate_series(1, 4) g'"

same "a function joined to a table" \
     "SELECT t0.a, g FROM t0 JOIN generate_series(2, 4) g ON g = t0.a ORDER BY 1"

declined "a function reading a column of the table beside it" \
         "SELECT t0.a, g FROM t0, generate_series(1, t0.a) g WHERE t0.a < 3 ORDER BY 1, 2" "LATERAL"

declined "WITH ORDINALITY" \
         "SELECT * FROM generate_series(1, 2) WITH ORDINALITY" "WITH ORDINALITY"

# --- foreign tables -------------------------------------------------------------------
#
# The foreign-data wrapper plans its own scan, and ORCA has to let it: the
# port hands it a PlannerInfo it can plan with, where Cloudberry patched
# clausesel.c to cope with one it could not.

if [ "$(q "SELECT count(*) FROM pg_available_extensions WHERE name IN ('file_fdw', 'postgres_fdw')")" = 2 ]; then
	printf '%s\n' "a,b" "1,one" "2,two" "3,three" > "$WORK/t2.csv"
	q "CREATE EXTENSION file_fdw; CREATE EXTENSION postgres_fdw;
	   CREATE SERVER t2_file FOREIGN DATA WRAPPER file_fdw;
	   CREATE FOREIGN TABLE t2_ft (a int, b text) SERVER t2_file
	     OPTIONS (filename '$WORK/t2.csv', format 'csv', header 'true');
	   CREATE SERVER t2_loop FOREIGN DATA WRAPPER postgres_fdw
	     OPTIONS (host '$SOCK', port '$PORT', dbname 'postgres');
	   CREATE USER MAPPING FOR CURRENT_USER SERVER t2_loop;
	   CREATE TABLE t2_remote AS SELECT g AS a, 'r' || g AS b FROM generate_series(1, 50) g;
	   CREATE FOREIGN TABLE t2_pft (a int, b text) SERVER t2_loop OPTIONS (table_name 't2_remote');" > /dev/null

	shape "a file_fdw table" "Foreign File:" \
	      "SELECT * FROM t2_ft WHERE a > 1 ORDER BY a"

	shape "a postgres_fdw table, with the filter sent to the remote server" \
	      "Remote SQL: SELECT a, b FROM public.t2_remote WHERE ((a > 45))" \
	      "SELECT * FROM t2_pft WHERE a > 45 ORDER BY a"

	# One AND of two conditions, which the planner hands a wrapper as two:
	# as one, it failed an assertion in make_restrictinfo().
	shape "an AND of conditions, split as the planner splits it" \
	      "WHERE ((b ~~ 'r1%')) AND (((a % 2) = 0))" \
	      "SELECT b FROM t2_pft WHERE b LIKE 'r1%' AND a % 2 = 0 ORDER BY 1"

	# Past five custom plans the plan cache keeps a generic one, and the
	# parameter goes to the remote server as one.
	got=$(q "PREPARE t2_fq(int) AS SELECT b FROM t2_pft WHERE a = \$1;
	         EXECUTE t2_fq(1); EXECUTE t2_fq(2); EXECUTE t2_fq(3); EXECUTE t2_fq(4); EXECUTE t2_fq(5);
	         EXPLAIN (COSTS OFF, VERBOSE) EXECUTE t2_fq(6)")
	case "$got" in
		*'(a = $1::integer)'*"Optimizer: GPORCA"*) ok "a generic plan sends its parameter" ;;
		*) notok "a generic plan sends its parameter" "$got" ;;
	esac

	same "a foreign table joined to a local one" \
	     "SELECT p.a, f.b, p.b FROM t2_pft p JOIN t2_ft f ON p.a = f.a ORDER BY 1"

	declined "and one as the target, as in Cloudberry" \
	         "INSERT INTO t2_pft VALUES (100, 'x')" "Inserts with foreign tables" \
	         "SELECT"
else
	echo "  (file_fdw or postgres_fdw is not installed; foreign tables not tested)"
fi

# --- an incremental view, maintained under ORCA ----------------------------------------
#
# gp_matview maintains a view from inside the statement that changed its base
# table, with queries it builds and hands the planner -- ORCA, here.  One puts
# a subquery in a table's place and leaves the table's permission entry,
# which PostgreSQL's planner never reads and ORCA's permission check handed to
# ExecCheckPermissions(), which asserts that every entry is used: the first
# maintenance under ORCA stopped an assert-enabled server.

q "CREATE EXTENSION gp_matview CASCADE;
   CREATE TABLE t2_base (id int, grp int, amt numeric);
   INSERT INTO t2_base VALUES (1,1,10),(2,1,20),(3,2,30);
   CREATE MATERIALIZED VIEW t2_ivm WITH (gp.incremental) AS
     SELECT grp, count(*) AS n, sum(amt) AS total FROM t2_base GROUP BY grp;" > /dev/null

has "a change to an incremental view's table is ORCA's to plan" \
    "EXPLAIN (COSTS OFF) UPDATE t2_base SET amt = 1 WHERE id = 2" "Optimizer: GPORCA"

is "and the view follows an INSERT, a DELETE and an UPDATE" \
   "INSERT INTO t2_base VALUES (4,2,5); DELETE FROM t2_base WHERE id = 1;
    UPDATE t2_base SET amt = 100 WHERE id = 2;
    SELECT string_agg(t::text, ' ' ORDER BY t::text) FROM (SELECT * FROM t2_ivm) t" \
   "(1,1,100) (2,2,35)"

# --- CREATE TABLE AS -----------------------------------------------------------------
#
# On one node it is planned as the query it is; ORCA's CTAS operator, which
# plans the new table's distribution with it, is M2's.

has "CREATE TABLE AS is planned by ORCA as its query" \
    "EXPLAIN (COSTS OFF) CREATE TABLE t2_ctas AS SELECT a, b FROM t0 WHERE a < 10" "Optimizer: GPORCA"

is "and writes that query's rows" \
   "CREATE TEMP TABLE t2_ctas AS SELECT a, b FROM t0 WHERE a < 10; SELECT count(*), sum(a) FROM t2_ctas" "9|45"

is "and so does a materialized view, and its refresh" \
   "CREATE MATERIALIZED VIEW t2_mv AS SELECT b, count(*) n FROM t0 GROUP BY b;
    INSERT INTO t0 VALUES (-5, 'v1', 0, NULL); REFRESH MATERIALIZED VIEW t2_mv;
    SELECT n FROM t2_mv WHERE b = 'v1'; DELETE FROM t0 WHERE a = -5; DROP MATERIALIZED VIEW t2_mv" "101"

echo
echo "24. the partitions of a table, and the choice of them while the query runs, planned by ORCA"

# T3 of the translator.  ORCA scans a partitioned table with a dynamic scan
# of the partitions its static pruning left, and chooses among them again
# while the query runs with a Partition Selector on the other side of a hash
# join.  The port plans the scan ORCA chose for the table, makes a scan of
# each partition from it, and puts them under a Dynamic Scan, a CustomScan
# that runs the ones the selector chose.

q "CREATE TABLE t3p (a int, b text, c int) PARTITION BY RANGE (a);
   CREATE TABLE t3p1 PARTITION OF t3p FOR VALUES FROM (0) TO (100);
   CREATE TABLE t3p2 (c int, gone int, b text, a int);
   ALTER TABLE t3p2 DROP COLUMN gone;
   ALTER TABLE t3p ATTACH PARTITION t3p2 FOR VALUES FROM (100) TO (200);
   CREATE TABLE t3p3 PARTITION OF t3p FOR VALUES FROM (200) TO (300);
   CREATE TABLE t3pdef PARTITION OF t3p DEFAULT;
   INSERT INTO t3p SELECT g, 'v' || g, g % 7 FROM generate_series(0, 299) g;
   INSERT INTO t3p VALUES (NULL, 'null', 1), (500, 'big', 2), (-5, 'neg', 3);
   CREATE INDEX ON t3p (c);
   CREATE TABLE t3l (k text, v int) PARTITION BY LIST (k);
   CREATE TABLE t3l_ab PARTITION OF t3l FOR VALUES IN ('a', 'b');
   CREATE TABLE t3l_c PARTITION OF t3l FOR VALUES IN ('c', NULL);
   CREATE TABLE t3l_def PARTITION OF t3l DEFAULT;
   INSERT INTO t3l SELECT (ARRAY['a','b','c','d',NULL])[1 + g % 5], g FROM generate_series(1, 100) g;
   CREATE TABLE t3h (id int, v text) PARTITION BY HASH (id);
   CREATE TABLE t3h0 PARTITION OF t3h FOR VALUES WITH (MODULUS 3, REMAINDER 0);
   CREATE TABLE t3h1 PARTITION OF t3h FOR VALUES WITH (MODULUS 3, REMAINDER 1);
   CREATE TABLE t3h2 PARTITION OF t3h FOR VALUES WITH (MODULUS 3, REMAINDER 2);
   INSERT INTO t3h SELECT g, 'h' || g FROM generate_series(1, 90) g;
   CREATE TABLE t3j (a int, x int);
   INSERT INTO t3j SELECT g, g % 5 FROM generate_series(0, 299, 3) g;
   INSERT INTO t3j VALUES (NULL, 1), (500, 2);
   CREATE TABLE t3k (a int); INSERT INTO t3k SELECT g FROM generate_series(150, 160) g;
   CREATE TABLE t3tl (k text); INSERT INTO t3tl VALUES ('a'), ('c'), (NULL);" > /dev/null
q "VACUUM ANALYZE t3p;" > /dev/null
q "VACUUM ANALYZE t3l;" > /dev/null
q "VACUUM ANALYZE t3h;" > /dev/null
q "ANALYZE t3j; ANALYZE t3k; ANALYZE t3tl;" > /dev/null

# --- the partitions ORCA's static pruning left ------------------------------

shape "a scan of a partitioned table is a scan of each partition" "Dynamic Seq Scan on public.t3p" \
      "SELECT count(*), sum(a) FROM t3p"

# Not "a < 50", which the default partition can hold too.
has "and only of the ones static pruning left" \
    "EXPLAIN (COSTS OFF) SELECT count(*) FROM t3p WHERE a BETWEEN 10 AND 50" \
    "Number of partitions to scan: 1 (out of 4)"

# A partition can have its columns in another order, or a dropped one: each
# scan reads its own columns by name.
same "a partition whose columns are in another order, or dropped" \
     "SELECT a, b, c FROM t3p WHERE a IN (1, 150, 250, 500, -5) ORDER BY a"

same "tableoid names the partition a row came from" \
     "SELECT tableoid::regclass, count(*) FROM t3p GROUP BY 1 ORDER BY 1"

same "the default partition, and a NULL key" \
     "SELECT a, b FROM t3p WHERE a IS NULL OR a > 400 OR a < 0 ORDER BY a NULLS FIRST"

shape "each partition through its own index" "Index Scan using t3p2_c_idx" \
      "SELECT count(*) FROM t3p WHERE c = 3 AND a < 150"

# On a table of its own: an index on t3p's join column would take the hash
# join, and its Partition Selector, from the tests below.
q "CREATE TABLE t3q (a int, b text) PARTITION BY RANGE (a);
   CREATE TABLE t3q1 PARTITION OF t3q FOR VALUES FROM (0) TO (150);
   CREATE TABLE t3q2 PARTITION OF t3q FOR VALUES FROM (150) TO (300);
   INSERT INTO t3q SELECT g, 'q' || g FROM generate_series(0, 299) g;
   CREATE INDEX ON t3q (a);" > /dev/null
q "VACUUM ANALYZE t3q;" > /dev/null

shape "an index-only scan of each, under a nested loop" "Index Only Scan using t3q2_a_idx" \
      "SELECT count(*) FROM t3q JOIN t3k ON t3q.a = t3k.a"

shape "a bitmap scan of each" "Bitmap Index Scan on t3p3_c_idx" \
      "SELECT count(*) FROM t3p WHERE c = 3 OR c = 5" \
      "SET gp.optimizer_enable_dynamictablescan = off; SET gp.optimizer_enable_dynamicindexscan = off;
       SET gp.optimizer_enable_dynamicindexonlyscan = off"

same "a list-partitioned table, NULL and the default partition among them" \
     "SELECT k, count(*) FROM t3l GROUP BY k ORDER BY k NULLS FIRST"

same "a hash-partitioned table" \
     "SELECT count(*), min(v), max(v) FROM t3h WHERE id IN (7, 8, 9)"

same "a partitioned table read twice" \
     "SELECT count(*) FROM t3p x JOIN t3p y ON x.a = y.a + 1"

# Under a nested loop the partitions' scans are rescanned for each row.
same "and rescanned under a nested loop" \
     "SELECT t3j.a, (SELECT count(*) FROM t3p WHERE t3p.a = t3j.a) FROM t3j WHERE t3j.a < 20 ORDER BY 1"

# --- the choice while the query runs --------------------------------------------
#
# A Partition Selector on a hash join's inner side works out, from the rows it
# passes on, which partitions can hold a match, and the Dynamic Scan on the
# outer side runs only those.  The hash join has to build its hash table
# before it reads the outer side for that to happen, and PostgreSQL 19's
# decides by the costs.

shape "a Partition Selector chooses the partitions a join can match" "Partition Selector (selector id: \$" \
      "SELECT count(*) FROM t3p JOIN t3k ON t3p.a = t3k.a"

has "and the Dynamic Scan reads only the one it chose, after it chose" \
    "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
       SELECT count(*) FROM t3p JOIN t3k ON t3p.a = t3k.a" "Partitions Scanned: 1"

has "a selector that saw no row chooses no partition" \
    "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
       SELECT count(*) FROM t3p JOIN t3k ON t3p.a = t3k.a AND t3k.a > 1000" "Partitions Scanned: 0"

same "a join on a list partition's key, NULLs and all" \
     "SELECT count(*) FROM t3l JOIN t3tl ON t3l.k = t3tl.k"

same "a join on a hash partition's key" \
     "SELECT count(*) FROM t3h JOIN t3j ON t3h.id = t3j.a"

same "a subquery's value, which ORCA selects by as a join" \
     "SELECT count(*) FROM t3p WHERE a = (SELECT max(a) FROM t3k)"

same "a semi-join, an anti-join, and a join the table is outer to" \
     "SELECT (SELECT count(*) FROM t3p WHERE a IN (SELECT a FROM t3j WHERE x = 2)),
             (SELECT count(*) FROM t3p WHERE NOT EXISTS (SELECT 1 FROM t3j WHERE t3j.a = t3p.a)),
             (SELECT count(*) FROM t3j LEFT JOIN t3p ON t3p.a = t3j.a)"

# --- what is refused ---------------------------------------------------------------

# WHERE CURRENT OF looks for the scan of the table under the cursor's plan,
# inside an Append but not inside a CustomScan.
declined "a cursor over the partitions of a table, which WHERE CURRENT OF could not see into" \
         "DECLARE t3c CURSOR FOR SELECT a FROM t3p WHERE a < 5" \
         "a cursor over the partitions of a table" "BEGIN"

q "CREATE TABLE t3m (y int, mo int, v int) PARTITION BY RANGE (y);
   CREATE TABLE t3m2020 PARTITION OF t3m FOR VALUES FROM (2020) TO (2021) PARTITION BY LIST (mo);
   CREATE TABLE t3m2020a PARTITION OF t3m2020 FOR VALUES IN (1, 2, 3);
   CREATE TABLE t3m2021 PARTITION OF t3m FOR VALUES FROM (2021) TO (2022);
   INSERT INTO t3m VALUES (2020, 1, 1), (2021, 5, 2);" > /dev/null

declined "a table partitioned on more than one level, as in Cloudberry" \
         "SELECT count(*), sum(v) FROM t3m" "Multi-level partitioned tables"

if [ "$(q "SELECT count(*) FROM pg_available_extensions WHERE name = 'file_fdw'")" = 1 ]; then
	printf '%s\n' "300,f300" "301,f301" > "$WORK/t3.csv"
	q "CREATE EXTENSION IF NOT EXISTS file_fdw; CREATE SERVER t3_file FOREIGN DATA WRAPPER file_fdw;
	   CREATE TABLE t3f (a int, b text) PARTITION BY RANGE (a);
	   CREATE TABLE t3f1 PARTITION OF t3f FOR VALUES FROM (0) TO (300);
	   CREATE FOREIGN TABLE t3f2 PARTITION OF t3f FOR VALUES FROM (300) TO (400)
	     SERVER t3_file OPTIONS (filename '$WORK/t3.csv', format 'csv');
	   INSERT INTO t3f1 VALUES (1, 'h1'), (2, 'h2');" > /dev/null

	# A foreign partition's scan is planned by its wrapper, which needs a
	# permission entry of the partition's own; a partition read through its
	# table has none, and should not be given one.
	declined "a foreign partition" \
	         "SELECT count(*) FROM t3f" "DynamicForeignScan"
fi

echo
echo "25. what EXPLAIN calls the nodes of ORCA's plans, through O4"

# Assert, the dynamic scans and the Partition Selector are CustomScans here,
# because PostgreSQL 19 has no such nodes and a module cannot add one, and
# EXPLAIN prints a CustomScan as "Custom Scan (name)" unless the module that
# owns it names it.  O4, explain_node_label_hook, lets it; gp_orca names them
# as Cloudberry's EXPLAIN does, so that its users read the plan they know and
# its expected test output can carry over.  Each check reads one whole line.

# explains <name> <query> <line> [setup] [options]: ORCA planned it, and a
# line of its EXPLAIN reads exactly that, the indentation and arrow aside.
explains() {
	local setup="${4:-SELECT}" opts="${5:-COSTS OFF}" plan
	plan=$(q2 "$setup" "EXPLAIN ($opts) $2")
	case "$plan" in
		*"Optimizer: GPORCA"*) ;;
		*) notok "$1" "not planned by ORCA: $(printf '%s' "$plan" | tail -3 | tr '\n' '|')"; return ;;
	esac
	printf '%s\n' "$plan" | sed 's/^ *//; s/^->  //' | grep -qxF -- "$3" \
		&& ok "$1" || notok "$1" "no line [$3] in: $(printf '%s' "$plan" | tr '\n' '|')"
}

explains "a scan of a partitioned table is a Dynamic Seq Scan on it" \
         "SELECT count(*), sum(a) FROM t3p" "Dynamic Seq Scan on t3p"

explains "which says how many partitions it scans, out of how many" \
         "SELECT count(*), sum(a) FROM t3p" "Number of partitions to scan: 4 (out of 4)"

explains "under the name the query gave the table, as a scan's target is named" \
         "SELECT count(*) FROM t3p x WHERE x.b = 'v1'" "Dynamic Seq Scan on t3p x"

explains "and with its schema under VERBOSE" \
         "SELECT count(*) FROM t3p x WHERE x.b = 'v1'" "Dynamic Seq Scan on public.t3p x" \
         "SELECT" "COSTS OFF, VERBOSE"

explains "a Dynamic Index Scan names the table's index, then the table" \
         "SELECT count(*) FROM t3p WHERE c = 3 AND a < 150" "Dynamic Index Scan on t3p_c_idx on t3p"

explains "and so does a Dynamic Index Only Scan" \
         "SELECT count(*) FROM t3q JOIN t3k ON t3q.a = t3k.a" "Dynamic Index Only Scan on t3q_a_idx on t3q"

explains "a Dynamic Bitmap Heap Scan names the table" \
         "SELECT count(*) FROM t3p WHERE c = 3 OR c = 5" "Dynamic Bitmap Heap Scan on t3p" \
         "SET gp.optimizer_enable_dynamictablescan = off; SET gp.optimizer_enable_dynamicindexscan = off;
          SET gp.optimizer_enable_dynamicindexonlyscan = off"

explains "an Assert is an Assert" \
         "SELECT (SELECT b FROM t2a WHERE a = 2) FROM t2a" "Assert"

# The table counts as used, as an Append's does, so a condition over its
# columns names it -- Cloudberry prints "Hash Cond: (pt.ptid = t.tid)" -- and
# its partitions' scans take the names after it.  Before O4 was used the
# table had no name in the plan at all, and this line read "(a = t3k.a)".
explains "a condition over the table's columns names the table" \
         "SELECT count(*) FROM t3p JOIN t3k ON t3p.a = t3k.a" "Hash Cond: (t3p.a = t3k.a)"

orca=$(q "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM t3p" | grep -o 'Seq Scan on [a-z0-9]* [a-z0-9_]*$')
pg=$(q2 "SET gp.optimizer = off" "EXPLAIN (COSTS OFF) SELECT count(*), sum(a) FROM t3p" | grep -o 'Seq Scan on [a-z0-9]* [a-z0-9_]*$')
[ -n "$orca" ] && [ "$orca" = "$pg" ] \
	&& ok "the partitions' scans are named as under the planner's Append" \
	|| notok "the partitions' scans are named as under the planner's Append" "orca [$orca], planner [$pg]"

# A Partition Selector is named by the parameter it hands its choice over
# in, and the Dynamic Scan names the selectors it takes a choice from by the
# same parameter, which is how a reader matches the two up.
plan=$(q "EXPLAIN (COSTS OFF) SELECT count(*) FROM t3p JOIN t3k ON t3p.a = t3k.a")
sel=$(printf '%s\n' "$plan" | sed -n 's/.*->  Partition Selector (selector id: \(\$[0-9]*\))$/\1/p')
used=$(printf '%s\n' "$plan" | sed -n 's/^ *Partition Selectors: \(.*\)$/\1/p')
[ -n "$sel" ] && [ "$sel" = "$used" ] \
	&& ok "a Partition Selector's id is the one its Dynamic Scan names" \
	|| notok "a Partition Selector's id is the one its Dynamic Scan names" "selector [$sel], scan [$used]: $(printf '%s' "$plan" | tr '\n' '|')"

got=$(q "EXPLAIN (COSTS OFF) SELECT count(*) FROM t3p JOIN t3k ON t3p.a = t3k.a;
         EXPLAIN (COSTS OFF) SELECT count(*) FROM t3p WHERE c = 3 AND a < 150;
         EXPLAIN (COSTS OFF) SELECT (SELECT b FROM t2a WHERE a = 2) FROM t2a")
case "$got" in
	*"Custom Scan"*) notok "no node of ORCA's plans is left a Custom Scan" "$(printf '%s' "$got" | tr '\n' '|')" ;;
	*) ok "no node of ORCA's plans is left a Custom Scan" ;;
esac

# Only text output prints a node's name, so in the other formats each node
# stays a Custom Scan, with the plan provider's name beside it, and says what
# the name says as properties -- the ones EXPLAIN writes for a scan's target
# and index, and Cloudberry's "Selector ID".  The paths are strict because
# a lax ".**" reaches each object twice, once more through the array around
# it.
q "CREATE FUNCTION t25_json(query text) RETURNS jsonb LANGUAGE plpgsql AS \$\$
   DECLARE j json;
   BEGIN EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || query INTO j; RETURN j::jsonb; END \$\$;" > /dev/null

T25J="SELECT count(*) FROM t3p JOIN t3k ON t3p.a = t3k.a"
is "in JSON a Dynamic Scan is still a Custom Scan, and names the table" \
   "SELECT n->>'Node Type', n->>'Relation Name', n->>'Alias', n->>'Number of partitions to scan'
      FROM jsonb_path_query(t25_json('$T25J'), 'strict \$.** ? (@.\"Custom Plan Provider\" == \"Dynamic Scan\")') n" \
   "Custom Scan|t3p|t3p|4"

is "and the selector it takes a choice from is the one whose id that is" \
   "SELECT (SELECT n->'Partition Selectors'->>0
              FROM jsonb_path_query(t25_json('$T25J'), 'strict \$.** ? (@.\"Custom Plan Provider\" == \"Dynamic Scan\")') n)
         = (SELECT '\$' || (n->>'Selector ID')
              FROM jsonb_path_query(t25_json('$T25J'), 'strict \$.** ? (@.\"Custom Plan Provider\" == \"Partition Selector\")') n)" \
   "t"

is "an index scan's index is named, as ExplainIndexScanDetails names one" \
   "SELECT n->>'Index Name'
      FROM jsonb_path_query(t25_json('SELECT count(*) FROM t3p WHERE c = 3 AND a < 150'),
                            'strict \$.** ? (@.\"Custom Plan Provider\" == \"Dynamic Scan\")') n" \
   "t3p_c_idx"

# A CustomScan that is not ORCA's is its own module's to name.  gp_probe,
# the hook tests' module, puts one over every plan when armed and names it
# the way Cloudberry names a Motion; loaded before gp_orca, its label hook is
# the one gp_orca's has to pass that node on to.  So the server comes back
# with both, and each module's nodes are called what that module calls them.
{
	echo "shared_preload_libraries = 'gp_probe,gp_core,gp_orca,gp_matview'"
} >> "$WORK/data/postgresql.conf"
if "$BINDIR/pg_ctl" -D "$WORK/data" -l "$WORK/log" -w -t 60 restart > /dev/null 2>&1; then
	q "CREATE EXTENSION gp_probe;" > /dev/null

	got=$(q2 "SET gp.optimizer = off; SELECT gp_probe.arm_explain(true)" "EXPLAIN (COSTS OFF) SELECT 1")
	case "$got" in
		*"Probe Motion 3:1  (slice1; segments: 3)"*) ok "another module's CustomScan is left to it to name" ;;
		*) notok "another module's CustomScan is left to it to name" "$(printf '%s' "$got" | tr '\n' '|')" ;;
	esac

	explains "and ORCA's are still ORCA's to name beside it" \
	         "SELECT count(*), sum(a) FROM t3p" "Dynamic Seq Scan on t3p"
else
	notok "the server comes back with gp_probe loaded beside gp_orca" "$(tail -5 "$WORK/log" | tr '\n' '|')"
fi

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
