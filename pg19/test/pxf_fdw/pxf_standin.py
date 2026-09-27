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
# pxf_standin.py <port> <root>: a stand-in for PXF, for the pxf_fdw suite.
#
# It answers the three requests pxf_fdw makes of PXF's REST service, over
# the files of a directory: a table's resource is a directory under <root>,
# and each file in it a fragment.
#
#   GET  /pxf/v15/Fragmenter/getFragments  the fragments of X-GP-DATA-DIR
#   GET  /pxf/v15/Bridge                   the data of fragment X-GP-DATA-DIR
#   POST /pxf/v15/Writable/stream          rows, chunked, appended to a file of
#                                          X-GP-DATA-DIR's, one for each
#                                          statement and segment
#
# Each request is logged to <root>/requests.log -- its kind, the segment
# that made it (X-GP-SEGMENT-ID), its fragment and its filter -- so that the
# suite sees which node read what.  PXF itself reads Hadoop's, a database's
# or an object store's data, whatever its profile says; here every resource
# is text or CSV as it is on disk.

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PREFIX = "/pxf/v15/"


class StandIn(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def record(self, kind, detail=""):
        with open(os.path.join(ROOT, "requests.log"), "a") as log:
            log.write("%s segment=%s %s filter=%s\n" % (
                kind, self.headers.get("X-GP-SEGMENT-ID", "?"), detail,
                self.headers.get("X-GP-FILTER", "")))

    def reply(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def resource(self):
        return os.path.join(ROOT, self.headers["X-GP-DATA-DIR"].lstrip("/"))

    def do_GET(self):
        if self.path.startswith(PREFIX + "Fragmenter/getFragments"):
            directory = self.resource()
            names = sorted(os.listdir(directory)) if os.path.isdir(directory) else []
            fragments = [{"sourceName": os.path.join(self.headers["X-GP-DATA-DIR"], name),
                          "index": 0, "replicas": ["127.0.0.1"], "metadata": "",
                          "userData": None, "profile": None} for name in names]
            self.record("fragments", "count=%d" % len(fragments))
            self.reply(200, json.dumps({"PXFFragments": fragments}).encode())
        elif self.path.startswith(PREFIX + "Bridge"):
            path = self.resource()
            self.record("bridge", "fragment=%s" % self.headers["X-GP-DATA-DIR"])
            with open(path, "rb") as data:
                self.reply(200, data.read())
        else:
            self.reply(404, b"no such endpoint\n")

    def do_POST(self):
        if not self.path.startswith(PREFIX + "Writable/stream"):
            self.reply(404, b"no such endpoint\n")
            return
        body = b""
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            while True:
                size = int(self.rfile.readline().split(b";")[0].strip(), 16)
                if size == 0:
                    self.rfile.readline()
                    break
                body += self.rfile.read(size)
                self.rfile.readline()
        else:
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        directory = self.resource()
        os.makedirs(directory, exist_ok=True)
        name = "written-%s-%s" % (self.headers.get("X-GP-XID", "x").replace("/", "_"),
                                  self.headers.get("X-GP-SEGMENT-ID", "x"))
        with open(os.path.join(directory, name), "ab") as out:
            out.write(body)
        self.record("write", "bytes=%d" % len(body))
        self.reply(200, b"")


if __name__ == "__main__":
    ROOT = sys.argv[2]
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), StandIn).serve_forever()
