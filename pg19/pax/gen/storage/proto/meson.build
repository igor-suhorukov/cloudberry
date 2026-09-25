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

# PAX's protobuf messages, generated here so that they are
# storage/proto/<name>.pb.h under the build's pax/gen, which is where PAX
# includes them from.  They import one another by bare name, as Cloudberry's
# CMakeLists.txt generates them, from their own directory.
pax_proto_dir = meson.current_source_dir() / '..' / '..' / '..' / '..' / '..' / 'contrib' / 'pax_storage' / 'src' / 'cpp' / 'storage' / 'proto'
pax_proto_sources = custom_target('pax_proto',
  input: [pax_proto_dir / 'micro_partition_stats.proto',
          pax_proto_dir / 'pax.proto',
          pax_proto_dir / 'orc_proto.proto'],
  output: ['micro_partition_stats.pb.cc', 'micro_partition_stats.pb.h',
           'pax.pb.cc', 'pax.pb.h',
           'orc_proto.pb.cc', 'orc_proto.pb.h'],
  command: [protoc, '-I', pax_proto_dir, '--cpp_out=@OUTDIR@', '@INPUT@'],
)
