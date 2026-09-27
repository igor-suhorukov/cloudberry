/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * gpcontrib/cdb/cdbvars.h
 *	  The include overlay of gpcloud and pxf_fdw: what their files compiled
 *	  where they lie, and the port's copies of theirs, ask of Cloudberry's
 *	  cdbvars.h.
 *
 * gpcloud's C++ (gpcontrib/gpcloud/src) is compiled unchanged, with this
 * directory on its include path (meson.build).  s3conf.cpp reads which
 * segment reads a location's keys, GpIdentity.segindex, of the segments
 * getgpsegmentCount() counts, to take its share of them: gp_core's content
 * id here -- -1 on the coordinator and on one node, which read them all, as
 * Cloudberry's coordinator does.  getgpsegmentCount() and write_log(),
 * which it declares itself, are defined by the port's gpcloud.cpp.  pxf_fdw
 * names the node in its debug messages (pxf_fragment.h's PXF_SEGMENT_ID).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_COMPAT_GPCONTRIB_CDBVARS_H
#define GP_COMPAT_GPCONTRIB_CDBVARS_H

#include "gp_cluster.h"

typedef struct GpCloudIdentity
{
	int			segindex;		/* this node's content id */
} GpCloudIdentity;

static inline GpCloudIdentity
gpcloud_identity(void)
{
	GpCloudIdentity id;

	id.segindex = GpClusterContentId();
	return id;
}

#define GpIdentity (gpcloud_identity())

#endif							/* GP_COMPAT_GPCONTRIB_CDBVARS_H */
