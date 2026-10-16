#!/usr/bin/perl
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
# explain_only.pl <atmsort.pm>: a copy of Cloudberry's atmsort.pm that takes
# for an EXPLAIN, whose output the suites do not compare (--gpd_ignore_plans),
# only what is one, edited in place.
#
# atmsort takes a statement for an EXPLAIN when any line of it before its
# output has "explain" and, after it, a SELECT, INSERT, UPDATE, DELETE or
# EXECUTE -- a call's argument or a comment too -- and under
# --gpd_ignore_plans passes over the output that follows as a plan's: so
# select is_workfile_created('explain (analyze) select ...'), whose answer
# is whether an operator spilled, passed whatever it answered.  Cloudberry's
# own runs do not ignore plans, and compare such an answer as a plan.  Here
# a statement is taken for an EXPLAIN where it begins with one, or calls a
# function that returns a plan's lines -- one named for EXPLAIN, as
# explain_filter() and its kin are -- or counts what a plan is made of,
# which the suites do not compare as they compare no plan:
# count_index_scans() and get_selected_parts().  Any other answer is
# compared.  A line that is a comment takes no statement for one.
#
use strict;
use warnings;

my $file = shift or die "usage: explain_only.pl <atmsort.pm>\n";
open(my $in, '<', $file) or die "$file: $!\n";
my $src = do { local $/; <$in> };
close($in);

my $plan_call = '(?:\w+explain\w*|explain\w+|count_index_scans|get_selected_parts)\s*\(';
my @edits = (
	# EXPLAIN (COSTS OFF) ...
	[ 'if ($ini =~ m/explain\s*\(.*costs\s+(?:off|false|0).*\)/i)',
	  'if ($ini =~ m/^\s*explain\s*\(.*costs\s+(?:off|false|0).*\)/i)' ],
	# and any other
	[ 'if ($ini =~ m/explain.*(?:insert|update|delete|select|execute)/i)',
	  'if ($ini =~ m/^\s*explain\b.*(?:insert|update|delete|select|execute)/i ||'
	  . " (\$ini !~ m/^\\s*--/ && \$ini =~ m/\\b$plan_call/i))" ],
);
for my $e (@edits)
{
	my ($old, $new) = @$e;
	my $n = () = $src =~ /\Q$old\E/g;
	die "explain_only.pl: $file has $n lines of \"$old\", where one was looked for\n"
		unless $n == 1;
	$src =~ s/\Q$old\E/$new/;
}
open(my $out, '>', $file) or die "$file: $!\n";
print $out $src;
close($out);
