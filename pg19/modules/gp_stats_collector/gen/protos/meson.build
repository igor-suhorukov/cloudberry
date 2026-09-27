#-------------------------------------------------------------------------
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
#-------------------------------------------------------------------------

# gp_stats_collector's protobuf messages, generated here so that they are
# protos/<name>.pb.h under the build's gen directory, as its Makefile makes
# them under src: they import one another as protos/<name>.proto, from the
# module's directory.
gpsc_dir = meson.current_source_dir() / '..' / '..' / '..' / '..' / '..' / 'gpcontrib' / 'gp_stats_collector'
gpsc_proto_sources = custom_target('gpsc_proto',
  input: [gpsc_dir / 'protos' / 'gpsc_plan.proto',
          gpsc_dir / 'protos' / 'gpsc_metrics.proto',
          gpsc_dir / 'protos' / 'gpsc_set_service.proto'],
  output: ['gpsc_plan.pb.cc', 'gpsc_plan.pb.h',
           'gpsc_metrics.pb.cc', 'gpsc_metrics.pb.h',
           'gpsc_set_service.pb.cc', 'gpsc_set_service.pb.h'],
  command: [protoc, '-I', gpsc_dir, '--cpp_out=@OUTDIR@/..', '@INPUT@'],
)
