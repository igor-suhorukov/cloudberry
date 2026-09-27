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
 * gp_rtfilter.h
 *	  Cloudberry's runtime filters: a Bloom filter of a hash join's inner
 *	  keys, above its outer side and pushed down into the scans below it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_RTFILTER_H
#define GP_RTFILTER_H

extern void GpRtFilterInit(void);

#endif							/* GP_RTFILTER_H */
