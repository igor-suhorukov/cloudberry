#!/usr/bin/env python3
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
# s3data.py <endpoint> <access id> <secret>: the gpcloud suite's S3, made
# ready -- the buckets Cloudberry's regression tests read and write
# (regress/input/*.source), and the data they read, as its expected output
# describes it:
#
#   - the small data sets it gives line by line or by count -- baddata,
#     emptyfile, oneline, 1correct1wrong, csv_with_header, its CRLF copy and
#     csv_without_header, no_eol_at_eof;
#   - stand-ins for Cloudberry's own, which are not public, that give the
#     answers its expected output records: small17/data0000, in the bucket of
#     each of the thirteen regions the tests read it from (117,446 rows, whose
#     open sums to 4,239,338); the rest of ap-northeast-1's bucket, which a
#     test reads whole (a sum of 71,676,419 in all); 2001files (2,001
#     objects, 256,128 rows, open's sum 9,107,189); and normal/xac, whose
#     first line the error tests fail at, and which LIMIT 0 reads none of;
#   - the suite's own: small17/data0000 gzipped and deflated (a deflated
#     object is known by its name's .deflate, as gpcloud knows one), for the
#     tests of Cloudberry's that read its data sets so, which the suite does
#     not make; and 36 MB of rows, which gpcloud's threads read in three of
#     the configuration's chunks.
#
# Cloudberry's larger data sets -- normal/ (31 million rows), the airline
# data of five GB each, their gzipped and deflated copies -- are not made
# (manifest).

import gzip
import sys
import zlib

import boto3
from botocore.config import Config

ENDPOINT, ACCESSID, SECRETKEY = sys.argv[1], sys.argv[2], sys.argv[3]


def client(region):
    return boto3.client("s3", endpoint_url=ENDPOINT, region_name=region,
                        aws_access_key_id=ACCESSID, aws_secret_access_key=SECRETKEY,
                        config=Config(s3={"addressing_style": "path"}))


def bucket(name, region):
    s3 = client(region)
    if region == "us-east-1":
        s3.create_bucket(Bucket=name)
    else:
        s3.create_bucket(Bucket=name,
                         CreateBucketConfiguration={"LocationConstraint": region})
    return s3


def rows(n, opens, start=0):
    """n rows of the stock ticks' shape, the open of row i opens(i)."""
    return "".join("05/18/2010,09:%02d:%02d,%s,36.3,36.2,%d\n"
                   % ((i // 60) % 60, i % 60, opens(i), 100 + i)
                   for i in range(start, start + n))


def put(s3, name, key, body):
    s3.put_object(Bucket=name, Key=key, Body=body.encode() if isinstance(body, str) else body)


regions = ["us-east-1", "us-east-2", "us-west-1", "us-west-2", "ap-south-1",
           "ap-northeast-1", "ap-northeast-2", "ap-southeast-1", "ap-southeast-2",
           "eu-central-1", "eu-west-1", "sa-east-1"]

# small17/data0000: 117,446 rows, 72,318 opening at 36 and 45,128 at 36.25
small17 = rows(117446, lambda i: "36.25" if i < 45128 else "36")

main = bucket("s3test.pivotal.io", "us-west-2")
bucket("s3test.encrypt.pivotal.io", "us-west-2")
put(main, "s3test.pivotal.io", "regress/small17/data0000", small17)
for region in regions:
    s3 = bucket("%s.s3test.pivotal.io" % region, region)
    put(s3, "%s.s3test.pivotal.io" % region, "regress/small17/data0000", small17)

# ap-northeast-1's bucket read whole: 67,437 rows more, 81 of them at 1001
put(client("ap-northeast-1"), "ap-northeast-1.s3test.pivotal.io", "regress/rest/data0000",
    rows(67437, lambda i: "1001" if i < 81 else "1000"))

B = "s3test.pivotal.io"
# 7 rows and the 3 bad ones the error log shows, at lines 2 to 4
put(main, B, "regress/baddata/data0000",
    "09/28/2009,09:30:00,35.5,35.6,35.4,100\n"
    "10/05/2009,36.19,36.18,36.2,100\n"
    "09/28/2009,09:30:06,35.49,35.37\n"
    "10/05/2009,11:56:50,36.21,200\n"
    + rows(6, lambda i: "35.5"))
put(main, B, "regress/emptyfile/data0000", "")
put(main, B, "regress/oneline/data0000", rows(1, lambda i: "35.5"))
put(main, B, "regress/1correct1wrong/data0000",
    "09/28/2009,09:10:37,35.6,35.29,35.75,150\n"
    "whatever,09/28/2009,09:10:37,35.6,35.29,35.75,150,wherever\n")

# 17 rows in three files, each with its header line, in LF and in CRLF; and
# three rows without a header, whose volumes are three of theirs
HEADER = "date,time,open,high,low,volume\n"
for part, (start, n) in enumerate([(0, 6), (6, 6), (12, 5)]):
    body = HEADER + rows(n, lambda i: "35.5", start)
    put(main, B, "regress/csv_with_header/data%04d" % part, body)
    put(main, B, "regress/csv_with_header_crlf/data%04d" % part, body.replace("\n", "\r\n"))
put(main, B, "regress/csv_without_header/data0000", rows(3, lambda i: "35.5", 4))

# 36 rows in four files, none ending its last line
for part in range(4):
    put(main, B, "regress/no_eol_at_eof/data%04d" % part,
        "\n".join(str(part * 9 + i) for i in range(9)))

# 2001 objects of 128 rows, 142,709 of them opening at 36 and the rest at 35
for part in range(2001):
    put(main, B, "regress/2001files/data%04d" % part,
        rows(128, lambda i: "36" if i < 142709 else "35", part * 128))

# normal/xac: its first line is 05/18/2010's
put(main, B, "regress/normal/xac", rows(1000, lambda i: "35.5"))

# the suite's own
put(main, B, "regress/port/gzip/data0000.gz", gzip.compress(small17.encode()))
put(main, B, "regress/port/deflate/data0000.deflate", zlib.compress(small17.encode()))
put(main, B, "regress/port/chunks/data0000", rows(800000, lambda i: "1"))
