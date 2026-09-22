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
# canon.pl < unified diff > the same differences, in the form a reviewed one
# is kept in (orca/README).
#
# gpdiff.pl's unified diff, with zero lines of context, less what depends on
# the run rather than on the answers: the two file names, the line numbers of
# each hunk, and the lines of a plan -- gpdiff.pl's GP_IGNORE lines, which it
# leaves in a hunk beside a line that differs.  A hunk left with nothing is
# dropped.  What remains is each hunk's removed and added lines, in order,
# under a bare "@@".
#
# Bytes in, bytes out: the output is compared, not read.

use strict;
use warnings;

binmode STDIN;
binmode STDOUT;

my @hunk;
my $started = 0;

sub flush_hunk
{
	if (grep { /^[-+]/ } @hunk)
	{
		print "@@\n";
		print for @hunk;
	}
	@hunk = ();
}

while (my $line = <STDIN>)
{
	if ($line =~ /^@@ /)
	{
		flush_hunk() if $started;
		$started = 1;
		next;
	}
	next unless $started;				# the file names
	next if $line =~ /^[-+]GP_IGNORE:/;
	next if $line =~ /^\\ No newline/;
	push @hunk, $line;
}
flush_hunk() if $started;
