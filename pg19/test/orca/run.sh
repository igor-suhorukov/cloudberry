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

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
