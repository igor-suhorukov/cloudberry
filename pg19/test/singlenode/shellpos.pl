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
# shellpos.pl < results > the same, less the place PostgreSQL 19 gives a
# shell type in the NOTICE that says a function made one or uses one.
#
# "argument type t is only a shell" and "return type t is only a shell"
# carry the position of the type's name in the statement since PostgreSQL 17,
# a LINE and a caret, where Cloudberry's, of PostgreSQL 14, carry none.  The
# NOTICE is the same; the two lines under it are dropped from the results of
# Cloudberry's tests before they are compared.
#
# Bytes in, bytes out.

use strict;
use warnings;

binmode STDIN;
binmode STDOUT;

my $state = 0;					# 1 after the NOTICE, 2 after its LINE
while (my $line = <STDIN>)
{
	if ($state == 1 && $line =~ /^LINE \d+: /)
	{
		$state = 2;
		next;
	}
	if ($state == 2 && $line =~ /^\s*\^\s*$/)
	{
		$state = 0;
		next;
	}
	$state = ($line =~ /^NOTICE:  (?:argument|return) type .* is only a shell$/) ? 1 : 0;
	print $line;
}
