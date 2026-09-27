#!/usr/bin/env python3
# gen_core.py <joinrels.c> <allpaths.c> <core ref> > core.c: PostgreSQL 19's
# join search, copied for pg_hint_plan as its update_copied_funcs.pl copies it,
# the copies static, renamed where they are public, and their calls of
# make_join_rel() and add_paths_to_joinrel() the extension's.
#
# The two files are the core series' (github/postgres, REL_19_STABLE_CLOUDBERRY)
# at <core ref>, which core.c names; from that tree, for instance:
#   git show REF:src/backend/optimizer/path/joinrels.c > /tmp/joinrels.c
#   git show REF:src/backend/optimizer/path/allpaths.c > /tmp/allpaths.c
#   python3 gen_core.py /tmp/joinrels.c /tmp/allpaths.c REF > core.c
import re, sys

joinrels, allpaths, ref = sys.argv[1], sys.argv[2], sys.argv[3]
FUNCS = [
    (allpaths, 'set_plain_rel_pathlist'),
    (allpaths, 'create_plain_partial_paths'),
    (allpaths, 'standard_join_search'),
    (joinrels, 'join_search_one_level'),
    (joinrels, 'make_rels_by_clause_joins'),
    (joinrels, 'make_rels_by_clauseless_joins'),
    (joinrels, 'join_is_legal'),
    (joinrels, 'make_join_rel'),
    (joinrels, 'make_grouped_join_rel'),
    (joinrels, 'populate_joinrel_with_paths'),
    (joinrels, 'has_join_restriction'),
    (joinrels, 'restriction_is_constant_false'),
    (joinrels, 'try_partitionwise_join'),
    (joinrels, 'build_child_join_sjinfo'),
    (joinrels, 'free_child_join_sjinfo'),
    (joinrels, 'compute_partition_bounds'),
    (joinrels, 'get_matching_part_pairs'),
]
RENAME = {
    'standard_join_search': 'pg_hint_plan_standard_join_search',
    'join_search_one_level': 'pg_hint_plan_join_search_one_level',
    'make_join_rel': 'make_join_relation',
}

def extract(path, name):
    lines = open(path).read().split('\n')
    for i, l in enumerate(lines):
        if re.match(r'^' + re.escape(name) + r'\(', l):
            # the return type, "static" with it, on the line before
            start = i - 1
            # and the comment block above it
            j = start - 1
            if lines[j].strip() == '*/':
                while not lines[j].startswith('/*'):
                    j -= 1
                start = j
            # the body, to the closing brace in column 0
            k = i
            while lines[k] != '}':
                k += 1
            return lines[start:k + 1], lines[i - 1], i
    raise SystemExit('no function ' + name + ' in ' + path)

out = []
protos = []
for path, name in FUNCS:
    body, rettype, _ = extract(path, name)
    text = '\n'.join(body)
    newname = RENAME.get(name, name)
    # the definition: static, under its new name
    ret = rettype.strip()
    if not ret.startswith('static'):
        text = re.sub(r'^' + re.escape(rettype) + r'\n' + re.escape(name) + r'\(',
                      'static ' + ret + '\n' + newname + '(', text, count=1, flags=re.M)
    else:
        text = re.sub(r'^' + re.escape(name) + r'\(', newname + '(', text, count=1, flags=re.M)
    # its prototype, for the copies to call each other in any order
    m = re.search(r'^(static [^\n]*\n)' + re.escape(newname) + r'\(([^{]*?)\)\n\{', text, re.M)
    if m:
        head = m.group(1).rstrip('\n')
        protos.append(head + ('' if head.endswith('*') else ' ') + newname + '(' + m.group(2) + ');')
    out.append(text)

core = '\n\n'.join(out)
# the calls the hints take over
core = re.sub(r'\bmake_join_rel\(root,', 'pg_hint_plan_make_join_rel(root,', core)
core = re.sub(r'\badd_paths_to_joinrel\(root,', 'pg_hint_plan_add_paths_to_joinrel(root,', core)
core = re.sub(r'\bjoin_search_one_level\(root, lev\)', 'pg_hint_plan_join_search_one_level(root, lev)', core)

print('''/*-------------------------------------------------------------------------
 *
 * core.c
 *	  PostgreSQL 19's join search, copied for pg_hint_plan.
 *
 * Cloudberry's pg_hint_plan applies a join method hint around each join
 * relation its planner makes, through two hooks of its core that PostgreSQL
 * 19 has not, make_join_rel_hook and add_paths_to_joinrel_hook.  So, as the
 * pg_hint_plan of PostgreSQL's own releases does (its core.c, made by
 * update_copied_funcs.pl), these are the planner's functions from
 * standard_join_search() down to add_paths_to_joinrel(), and the two a scan
 * method hint makes a relation's paths again with, copied as they are but
 * for four things: each is static; standard_join_search() is
 * pg_hint_plan_standard_join_search(), join_search_one_level() is
 * pg_hint_plan_join_search_one_level(), and make_join_rel() is
 * make_join_relation(); and their calls of make_join_rel() and
 * add_paths_to_joinrel() are pg_hint_plan_make_join_rel() and
 * pg_hint_plan_add_paths_to_joinrel(), the hints' (pg_hint_plan.c), which
 * call make_join_relation() and add_paths_to_joinrel() in turn.
 *
 * Made by gen_core.py, beside it, from the core series at ''' + ref + ''':
 *	  src/backend/optimizer/path/allpaths.c: set_plain_rel_pathlist(),
 *		create_plain_partial_paths(), standard_join_search()
 *	  src/backend/optimizer/path/joinrels.c: join_search_one_level(),
 *		make_rels_by_clause_joins(), make_rels_by_clauseless_joins(),
 *		join_is_legal(), make_join_rel(), make_grouped_join_rel(),
 *		populate_joinrel_with_paths(), has_join_restriction(),
 *		restriction_is_constant_false(),
 *		try_partitionwise_join(), build_child_join_sjinfo(),
 *		free_child_join_sjinfo(), compute_partition_bounds(),
 *		get_matching_part_pairs()
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
''')
print('\n'.join(protos))
print()
print(core)
