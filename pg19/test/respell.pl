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
# A setting the port has spelled as the port spells it, in Cloudberry's
# tests and their expected output: the harnesses' respelling.
#
#   perl respell.pl SPEC [FILE...]      (the files, or stdin, to stdout)
#
# SPEC is a harness's, made once a run:
#
#   kinds field set func header     what is respelled, and in what order
#   map gp_role gp.role             Cloudberry's name, and the port's
#   sed s/PATTERN/REPLACEMENT/FLAGS a rule of sed -E of the harness's own,
#                                   applied after the kinds, in order
#
# Each kind is one pattern over every name the map has, where a harness
# used to give sed a rule a setting (a few hundred of them, which took sed
# minutes over a suite's tests): the same rules, applied a kind at a time
# rather than a setting at a time -- a name the port gave one setting no
# rule for another matches, so the order within a kind is not the answer's.
# The kinds, NAME a name of the map, GPNAME one that begins "gp_", each
# case-insensitive but for the last four:
#
#   field     r.NAME -> r."gp.name", a field of a row read from SHOW
#   set       SET|RESET|SHOW [LOCAL|SESSION] NAME
#   setsys    SET|RESET|SHOW [LOCAL|SESSION|SYSTEM] NAME
#   altersys  ALTER SYSTEM SET|RESET NAME
#   func      current_setting('NAME' and set_config('NAME'
#   nameeq    name = 'NAME'
#   showguc   show_guc('NAME')
#   like      '%NAME%'
#   header    a line of GPNAME between spaces, SHOW's header
#   header0   the same, the spaces optional

use strict;
use warnings;

my $spec = shift @ARGV or die "usage: respell.pl SPEC [FILE...]\n";
open(my $sf, '<', $spec) or die "respell.pl: $spec: $!\n";
my (@kinds, %map, @sed);
while (my $line = <$sf>)
{
	chomp $line;
	next if $line =~ /^\s*(#|$)/;
	if ($line =~ /^kinds\s+(.*)$/) { push @kinds, split(' ', $1); }
	elsif ($line =~ /^map\s+(\S+)\s+(\S+)$/) { $map{lc $1} = $2; }
	elsif ($line =~ /^sed\s+(.*)$/) { push @sed, $1; }
	else { die "respell.pl: $spec: what is \"$line\"?\n"; }
}
close $sf;

# Longest first, as the harnesses gave sed their settings.
my @names = sort { length($b) <=> length($a) || $a cmp $b } keys %map;
my $N = join('|', map { quotemeta } @names) || '(?!)';
my $G = join('|', map { quotemeta } grep { /^gp_/ } @names) || '(?!)';

my %kind = (
	field    => sub { $_[0] =~ s/\b([a-z_][a-z0-9_]*)\.($N)\b/$1."$map{lc $2}"/gi },
	set      => sub { $_[0] =~ s/\b(set|reset|show)(\s+(?:local|session)\s+|\s+)($N)\b/$1$2$map{lc $3}/gi },
	setsys   => sub { $_[0] =~ s/\b(set|reset|show)(\s+(?:local|session|system)\s+|\s+)($N)\b/$1$2$map{lc $3}/gi },
	altersys => sub { $_[0] =~ s/\b(alter\s+system\s+(?:set|reset)\s+)($N)\b/$1$map{lc $2}/gi },
	func     => sub { $_[0] =~ s/\b(current_setting|set_config)\('($N)'/$1('$map{lc $2}'/gi },
	nameeq   => sub { $_[0] =~ s/\b(name\s*=\s*)'($N)'/$1'$map{lc $2}'/gi },
	showguc  => sub { $_[0] =~ s/\bshow_guc\('($N)'\)/show_guc('$map{$1}')/g },
	like     => sub { $_[0] =~ s/'%($N)%'/'%$map{$1}%'/g },
	header   => sub { $_[0] =~ s/^( +)($G)( +)$/$1$map{$2}$3/ },
	header0  => sub { $_[0] =~ s/^( *)($G)( *)$/$1$map{$2}$3/ },
);
my @subs;
foreach my $k (@kinds)
{
	die "respell.pl: no kind \"$k\"\n" unless $kind{$k};
	push @subs, $kind{$k};
}

# A rule of sed -E as Perl, with the same delimiter: a group is $N where sed
# says \N, and sed's I flag Perl's i; the patterns are the same.  (A rule
# with sed's & for the whole match, or a $ or @ Perl would read as a
# variable where sed reads it as itself, would not be; none is.)
foreach my $rule (@sed)
{
	my @part;
	my $cur = '';
	die "respell.pl: not a substitution: $rule\n" unless $rule =~ /^s(.)/;
	my $d = $1;
	my $s = substr($rule, 2);
	while (length $s)
	{
		if ($s =~ s/^(\\.)//s) { $cur .= $1; }
		elsif (substr($s, 0, 1) eq $d) { $s = substr($s, 1); push @part, $cur; $cur = ''; }
		else { $s =~ s/^(.)//s; $cur .= $1; }
	}
	push @part, $cur;
	die "respell.pl: not a substitution: $rule\n" unless @part == 3;
	my ($pat, $rep, $flags) = @part;
	$rep =~ s/\\(\d)/\${$1}/g;
	$flags =~ tr/I/i/;
	die "respell.pl: flags \"$flags\" in $rule\n" if $flags =~ /[^gi]/;
	my $code = "sub { \$_[0] =~ s$d$pat$d$rep$d$flags }";
	my $sub = eval $code or die "respell.pl: $rule: $@\n";
	push @subs, $sub;
}

while (my $line = <>)
{
	$_->($line) foreach @subs;
	print $line;
}
