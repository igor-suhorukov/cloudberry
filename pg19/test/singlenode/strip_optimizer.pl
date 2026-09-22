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
# strip_optimizer.pl FILE
#
# FILE as psql would have printed it without the "Optimizer:" line gp_orca
# adds to every text-format EXPLAIN, which is the line Cloudberry's EXPLAIN
# ends with and PostgreSQL's expected output does not have.
#
# Taking the line out is not enough, because psql sized the table around it:
# it is often the widest line of a short plan, so the column's title and the
# rule under it are wider than PostgreSQL's, and it is counted in the "(N
# rows)" under the table.  So a table that loses the line is printed again,
# at the width of what is left, the title centred as psql centres it.  Where
# the line is not in a table -- a plan a function passed on in a message --
# it goes, and nothing else changes.
#
# The file is read and written as bytes, so that whatever a test printed
# comes out as it went in; only a width is measured in characters.

use strict;
use warnings;
use Encode ();

my $OPTIMIZER = qr/^ ?Optimizer: (?:GPORCA|Postgres query optimizer)\n\z/;
my $FOOTER = qr/^\((\d+) rows?\)\n\z/;

sub width
{
	my ($text) = @_;

	return length(Encode::decode('UTF-8', $text, Encode::FB_DEFAULT));
}

open(my $in, '<:raw', $ARGV[0]) or die "$ARGV[0]: $!";
my @lines = <$in>;
close $in;
binmode(STDOUT, ':raw');

my @out;
my $pending = 0;	# lines dropped outside an aligned table, for its count

for (my $i = 0; $i < @lines; $i++)
{
	# An aligned table of one column, as psql prints one: a title centred
	# in a line exactly as long as the rule of dashes under it, rows that
	# each begin with a space, and the count.
	if ($i + 1 < @lines && $lines[$i + 1] =~ /^-+\n\z/ &&
		width($lines[$i]) == width($lines[$i + 1]) &&
		$lines[$i] =~ /^ +\S/ && $lines[$i] !~ /\|/)
	{
		my $j = $i + 2;
		my @rows;

		push @rows, $lines[$j++] while ($j < @lines && $lines[$j] =~ /^ /);

		if ($j < @lines && $lines[$j] =~ $FOOTER &&
			grep { $_ =~ $OPTIMIZER } @rows)
		{
			my @kept = grep { $_ !~ $OPTIMIZER } @rows;
			(my $title = $lines[$i]) =~ s/^\s+|\s+\z//g;
			my $w = width($title);

			foreach my $row (@kept)
			{
				(my $value = $row) =~ s/^ //;
				chomp $value;
				$w = width($value) if width($value) > $w;
			}

			my $left = int(($w - width($title)) / 2);
			my $right = $w - width($title) - $left;
			my $n = ($lines[$j] =~ $FOOTER)[0] - (@rows - @kept);

			push @out, ' ' . (' ' x $left) . $title . (' ' x $right) . " \n";
			push @out, ('-' x ($w + 2)) . "\n";
			push @out, @kept;
			push @out, "($n " . ($n == 1 ? 'row' : 'rows') . ")\n";
			$i = $j;
			next;
		}
	}

	# Unaligned output, or a plan inside a message: the line alone, and one
	# fewer in the count under it, if a count comes before a blank line.
	if ($lines[$i] =~ $OPTIMIZER)
	{
		$pending++;
		next;
	}
	if ($pending && $lines[$i] =~ $FOOTER)
	{
		my $n = $1 - $pending;

		push @out, "($n " . ($n == 1 ? 'row' : 'rows') . ")\n";
		$pending = 0;
		next;
	}
	$pending = 0 if $lines[$i] eq "\n";

	push @out, $lines[$i];
}

print @out;
